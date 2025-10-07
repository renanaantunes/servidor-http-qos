#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BACKLOG 128
#define BUF_SIZE 8192
#define DEFAULT_PORT "8080"
#define DEFAULT_MAX_KBPS 10000
#define DEFAULT_UNKNOWN_KBPS 1000
#define DISPLAY_INTERVAL_SEC 1
#define DEFAULT_WWW_DIR "./www"

typedef struct client_info {
    char ip[64];
    int configured_kbps;      // from config file (or default)
    int active_conns;         // number of active connections for this IP
    long last_html_ts_ms;     // timestamp of last html request (ms)
    long rtt_ms;              // estimated RTT in ms (from two requests)
    struct client_info *next;
} client_info_t;

client_info_t *clients_head = NULL;
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

int server_max_kbps = DEFAULT_MAX_KBPS;
char *www_dir = NULL;
char *ip_file_path = NULL;
int running = 1;

// Utility: get current time in ms
long now_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
}

// Read ip rates from file into clients list (configured_kbps only)
void load_ip_rates(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("fopen ip rates");
        return;
    }
    char line[256];
    pthread_mutex_lock(&clients_lock);
    while (fgets(line, sizeof(line), f)) {
        char ip[64];
        int kbps;
        if (sscanf(line, "%63s %d", ip, &kbps) == 2) {
            // add or update
            client_info_t *c = clients_head;
            while (c) {
                if (strcmp(c->ip, ip) == 0) break;
                c = c->next;
            }
            if (!c) {
                c = calloc(1, sizeof(client_info_t));
                strncpy(c->ip, ip, sizeof(c->ip)-1);
                c->configured_kbps = kbps;
                c->active_conns = 0;
                c->last_html_ts_ms = 0;
                c->rtt_ms = 0;
                c->next = clients_head;
                clients_head = c;
            } else {
                c->configured_kbps = kbps;
            }
        }
    }
    pthread_mutex_unlock(&clients_lock);
    fclose(f);
}

// find or create client info for ip
client_info_t* get_or_create_client(const char *ip) {
    pthread_mutex_lock(&clients_lock);
    client_info_t *c = clients_head;
    while (c) {
        if (strcmp(c->ip, ip) == 0) {
            pthread_mutex_unlock(&clients_lock);
            return c;
        }
        c = c->next;
    }
    // not found -> create with default kbps (mark 0 if unknown, we'll set default on demand)
    c = calloc(1, sizeof(client_info_t));
    strncpy(c->ip, ip, sizeof(c->ip)-1);
    c->configured_kbps = 0; // 0 means not explicitly configured
    c->active_conns = 0;
    c->last_html_ts_ms = 0;
    c->rtt_ms = 0;
    c->next = clients_head;
    clients_head = c;
    pthread_mutex_unlock(&clients_lock);
    return c;
}

// compute total current allocated kbps (sum of configured_kbps for all active connections,
// but note we actually need to consider per-connection split)
int compute_current_used_kbps() {
    pthread_mutex_lock(&clients_lock);
    client_info_t *c = clients_head;
    int total = 0;
    while (c) {
        if (c->active_conns > 0) {
            int cfg = (c->configured_kbps > 0) ? c->configured_kbps : DEFAULT_UNKNOWN_KBPS;
            // each connection gets cfg / active_conns kbps
            total += cfg; // effectively count configured kbps per IP (total reserved per IP)
        }
        c = c->next;
    }
    pthread_mutex_unlock(&clients_lock);
    return total;
}

// helper: send an HTTP response header
ssize_t sendall(int sock, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = buf;
    while (total < len) {
        ssize_t sent = send(sock, p + total, len - total, 0);
        if (sent <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += sent;
    }
    return total;
}

int ends_with_html(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)
        return 1;
    return 0;
}

void respond_503(int client_sock) {
    const char *resp = "HTTP/1.1 503 Service Unavailable\r\n"
                       "Connection: close\r\n"
                       "Content-Length: 19\r\n"
                       "Content-Type: text/plain\r\n\r\n"
                       "503 Service Unavailable";
    sendall(client_sock, resp, strlen(resp));
}

void respond_404(int client_sock) {
    const char *resp = "HTTP/1.1 404 Not Found\r\n"
                       "Connection: close\r\n"
                       "Content-Length: 13\r\n"
                       "Content-Type: text/plain\r\n\r\n"
                       "404 Not Found";
    sendall(client_sock, resp, strlen(resp));
}

void respond_400(int client_sock) {
    const char *resp = "HTTP/1.1 400 Bad Request\r\n"
                       "Connection: close\r\n"
                       "Content-Length: 12\r\n"
                       "Content-Type: text/plain\r\n\r\n"
                       "400 Bad Request";
    sendall(client_sock, resp, strlen(resp));
}

// send headers for OK response (simple)
void send_file_headers(int client_sock, const char *content_type, off_t content_len, int keep_alive) {
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %lld\r\n"
        "Content-Type: %s\r\n"
        "%s"
        "\r\n",
        (long long)content_len,
        content_type ? content_type : "application/octet-stream",
        keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    sendall(client_sock, header, n);
}

// minimal content-type guess
const char* guess_content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".css") == 0) return "text/css";
    if (strcasecmp(dot, ".js") == 0) return "application/javascript";
    return "application/octet-stream";
}

// send file with rate limiting (kbps_per_conn). If kbps_per_conn <= 0 -> send as fast as possible.
int send_file_with_rate_limit(int sock, FILE *f, off_t file_len, int kbps_per_conn) {
    size_t chunk = 16 * 1024; // 16 KB chunk
    char *buf = malloc(chunk);
    if (!buf) return -1;
    size_t to_send;
    long long bytes_per_sec = 0;
    if (kbps_per_conn > 0) {
        bytes_per_sec = (long long)kbps_per_conn * 1000LL / 8LL; // convert kilobits/s -> bytes/s
        if (bytes_per_sec == 0) bytes_per_sec = 1;
    }
    off_t sent_total = 0;
    while ((to_send = fread(buf, 1, chunk, f)) > 0) {
        if (sendall(sock, buf, to_send) < 0) {
            free(buf);
            return -1;
        }
        sent_total += to_send;
        if (kbps_per_conn > 0) {
            // compute sleep time to respect bytes_per_sec
            double seconds = (double)to_send / (double)bytes_per_sec;
            if (seconds > 0) {
                // convert to microseconds
                long usec = (long)(seconds * 1e6);
                if (usec > 0) {
                    usleep(usec);
                }
            }
        }
    }
    free(buf);
    return 0;
}

// trim leading/trailing spaces
char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

// parse HTTP request line (very simple)
int parse_http_request_line(const char *req, char *method, size_t mm, char *path, size_t pm) {
    // expect "GET /path HTTP/1.1"
    const char *p = req;
    while (*p && *p != '\r' && *p != '\n') p++;
    size_t line_len = p - req;
    char line[1024];
    if (line_len >= sizeof(line)) return -1;
    memcpy(line, req, line_len);
    line[line_len] = '\0';
    char m[16], pth[512], ver[16];
    if (sscanf(line, "%15s %511s %15s", m, pth, ver) != 3) return -1;
    strncpy(method, m, mm-1);
    strncpy(path, pth, pm-1);
    return 0;
}

// worker thread for a connection
typedef struct worker_arg {
    int client_sock;
    char client_ip[64];
} worker_arg_t;

void *connection_handler(void *arg) {
    worker_arg_t *w = (worker_arg_t*)arg;
    int client_sock = w->client_sock;
    char client_ip[64];
    strncpy(client_ip, w->client_ip, sizeof(client_ip)-1);
    free(w);

    // find client record and increment active_conns
    client_info_t *c = get_or_create_client(client_ip);

    pthread_mutex_lock(&clients_lock);
    c->active_conns += 1;
    pthread_mutex_unlock(&clients_lock);

    // compute allowed kbps per connection (configured_kbps / active_conns)
    int configured_kbps = (c->configured_kbps > 0) ? c->configured_kbps : DEFAULT_UNKNOWN_KBPS;

    // VERY IMPORTANT: before fully accepting connection, re-check server admission control:
    int total_used_kbps_before = compute_current_used_kbps();
    // For admission, we consider that this IP's total configured_kbps will be counted (already counted)
    // Already incremented active_conns, so compute_current_used_kbps already includes this IP.
    if (total_used_kbps_before > server_max_kbps) {
        // reject connection
        respond_503(client_sock);
        close(client_sock);
        pthread_mutex_lock(&clients_lock);
        c->active_conns -= 1;
        pthread_mutex_unlock(&clients_lock);
        return NULL;
    }

    // read request (very simple, read up to header)
    char buffer[BUF_SIZE];
    ssize_t r = recv(client_sock, buffer, sizeof(buffer)-1, 0);
    if (r <= 0) {
        close(client_sock);
        pthread_mutex_lock(&clients_lock);
        c->active_conns -= 1;
        pthread_mutex_unlock(&clients_lock);
        return NULL;
    }
    buffer[r] = '\0';

    char method[16], path[512];
    if (parse_http_request_line(buffer, method, sizeof(method), path, sizeof(path)) < 0) {
        respond_400(client_sock);
        close(client_sock);
        pthread_mutex_lock(&clients_lock);
        c->active_conns -= 1;
        pthread_mutex_unlock(&clients_lock);
        return NULL;
    }
    // store timestamp for RTT estimation
    long ts = now_ms();
    if (ends_with_html(path)) {
        // first request for HTML -> store timestamp
        pthread_mutex_lock(&clients_lock);
        c->last_html_ts_ms = ts;
        pthread_mutex_unlock(&clients_lock);
    } else {
        // if not HTML and we have last_html_ts, compute RTT
        pthread_mutex_lock(&clients_lock);
        if (c->last_html_ts_ms > 0) {
            long delta = ts - c->last_html_ts_ms;
            if (delta > 0) c->rtt_ms = delta;
            c->last_html_ts_ms = 0; // reset after computing
        }
        pthread_mutex_unlock(&clients_lock);
    }

    // sanitize path: remove leading '/'
    char relpath[1024];
    if (path[0] == '/') strncpy(relpath, path+1, sizeof(relpath)-1);
    else strncpy(relpath, path, sizeof(relpath)-1);
    if (strlen(relpath) == 0) strcpy(relpath, "index.html");

    // build full path
    char fullpath[2048];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", www_dir, relpath);

    // check existence
    struct stat st;
    if (stat(fullpath, &st) < 0 || S_ISDIR(st.st_mode)) {
        respond_404(client_sock);
        close(client_sock);
        pthread_mutex_lock(&clients_lock);
        c->active_conns -= 1;
        pthread_mutex_unlock(&clients_lock);
        return NULL;
    }

    const char *ctype = guess_content_type(fullpath);
    // prepare headers
    send_file_headers(client_sock, ctype, st.st_size, 0);

    // compute per-connection kbps: configured_kbps / active_conns
    pthread_mutex_lock(&clients_lock);
    int active = c->active_conns;
    int cfg = (c->configured_kbps > 0) ? c->configured_kbps : DEFAULT_UNKNOWN_KBPS;
    pthread_mutex_unlock(&clients_lock);

    int kbps_per_conn = cfg / (active > 0 ? active : 1);

    // if file is HTML, do NOT throttle
    if (ends_with_html(fullpath)) {
        FILE *f = fopen(fullpath, "rb");
        if (!f) {
            respond_404(client_sock);
            close(client_sock);
            pthread_mutex_lock(&clients_lock);
            c->active_conns -= 1;
            pthread_mutex_unlock(&clients_lock);
            return NULL;
        }
        send_file_with_rate_limit(client_sock, f, st.st_size, 0); // 0 -> no limit
        fclose(f);
    } else {
        // Non-HTML: apply rate limiting
        FILE *f = fopen(fullpath, "rb");
        if (!f) {
            respond_404(client_sock);
            close(client_sock);
            pthread_mutex_lock(&clients_lock);
            c->active_conns -= 1;
            pthread_mutex_unlock(&clients_lock);
            return NULL;
        }
        send_file_with_rate_limit(client_sock, f, st.st_size, kbps_per_conn);
        fclose(f);
    }

    close(client_sock);
    pthread_mutex_lock(&clients_lock);
    c->active_conns -= 1;
    pthread_mutex_unlock(&clients_lock);
    return NULL;
}

// accept loop thread
void *accept_loop(void *arg) {
    int listen_fd = *(int*)arg;
    while (running) {
        struct sockaddr_storage their_addr;
        socklen_t addr_size = sizeof(their_addr);
        int new_fd = accept(listen_fd, (struct sockaddr*)&their_addr, &addr_size);
        if (new_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        // get peer IP
        char ipstr[INET6_ADDRSTRLEN];
        if (their_addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in*)&their_addr;
            inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6*)&their_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof(ipstr));
        }

        // quick admission control: compute sum of configured_kbps across active IPs (includes this one if exists)
        // We'll create/get client and increment active_conns inside worker to avoid races

        // spawn thread
        pthread_t tid;
        worker_arg_t *w = malloc(sizeof(worker_arg_t));
        w->client_sock = new_fd;
        strncpy(w->client_ip, ipstr, sizeof(w->client_ip)-1);
        if (pthread_create(&tid, NULL, connection_handler, w) != 0) {
            perror("pthread_create");
            close(new_fd);
            free(w);
        } else {
            pthread_detach(tid);
        }
    }
    return NULL;
}

// display thread: prints clients info periodically
void *display_thread(void *arg) {
    (void)arg;
    while (running) {
        sleep(DISPLAY_INTERVAL_SEC);
        pthread_mutex_lock(&clients_lock);
        printf("\n=== Clientes ativos (timestamp %ld) ===\n", now_ms());
        printf("%-16s %-8s %-10s %-8s\n", "IP", "Conns", "Config_kbps", "RTT(ms)");
        client_info_t *c = clients_head;
        while (c) {
            if (c->active_conns > 0) {
                int cfg = (c->configured_kbps > 0) ? c->configured_kbps : DEFAULT_UNKNOWN_KBPS;
                printf("%-16s %-8d %-10d %-8ld\n", c->ip, c->active_conns, cfg, c->rtt_ms);
            }
            c = c->next;
        }
        printf("Server max kbps: %d, used_kbps_est: %d\n", server_max_kbps, compute_current_used_kbps());
        pthread_mutex_unlock(&clients_lock);
    }
    return NULL;
}

void usage(const char *prog) {
    printf("Usage: %s [--port PORT] [--max-kbps N] [--ip-file path] [--www path]\n", prog);
}

int main(int argc, char **argv) {
    char *port = DEFAULT_PORT;
    www_dir = DEFAULT_WWW_DIR;
    ip_file_path = NULL;
    server_max_kbps = DEFAULT_MAX_KBPS;

    // simple args parse
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i], "--port") == 0 && i+1<argc) port = argv[++i];
        else if (strcmp(argv[i], "--max-kbps") == 0 && i+1<argc) server_max_kbps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ip-file") == 0 && i+1<argc) ip_file_path = argv[++i];
        else if (strcmp(argv[i], "--www") == 0 && i+1<argc) www_dir = argv[++i];
        else { usage(argv[0]); return 1; }
    }

    if (ip_file_path) load_ip_rates(ip_file_path);

    // create listen socket
    struct addrinfo hints, *res, *p;
    int listen_fd;
    int yes = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, port, &hints, &res) != 0) {
        perror("getaddrinfo");
        return 1;
    }
    for (p = res; p != NULL; p = p->ai_next) {
        listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listen_fd < 0) continue;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) < 0) { close(listen_fd); continue; }
        break;
    }
    if (p == NULL) {
        fprintf(stderr, "failed to bind\n");
        return 2;
    }
    freeaddrinfo(res);

    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        return 3;
    }
    printf("HTTP QoS Server listening on port %s, www_dir=%s, max_kbps=%d\n", port, www_dir, server_max_kbps);

    // spawn accept loop in thread
    pthread_t accept_tid;
    pthread_create(&accept_tid, NULL, accept_loop, &listen_fd);

    // spawn display thread
    pthread_t disp_tid;
    pthread_create(&disp_tid, NULL, display_thread, NULL);

    // simple signal handling: wait for ctrl-c
    signal(SIGINT, [](int sig){ (void)sig; running = 0; });

    // main thread sleeps while threads run
    while (running) sleep(1);

    printf("Shutting down...\n");
    close(listen_fd);
    pthread_join(accept_tid, NULL);
    pthread_join(disp_tid, NULL);

    // free client list
    pthread_mutex_lock(&clients_lock);
    client_info_t *it = clients_head;
    while (it) {
        client_info_t *nx = it->next;
        free(it);
        it = nx;
    }
    pthread_mutex_unlock(&clients_lock);

    return 0;
}
