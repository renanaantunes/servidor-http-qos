#define _GNU_SOURCE
#include <arpa/inet.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define BUF 16384
#define KBPS_DEFAULT 1000
#define MAX_KBPS_SERVER 10000
#define DISPLAY_INTERVAL 2

typedef struct Client {
    char ip[64];
    int kbps_cfg;             // Taxa configurada via ip_file
    int conns;                // Conexões ativas desse IP
    unsigned long last_html;  // Timestamp da última requisição HTML
    unsigned long rtt;        // RTT estimado
    struct Client *next;
} Client;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
Client *clients = NULL;
int port = 8080, max_kbps = MAX_KBPS_SERVER;
char www[256] = "./www";
char ip_file[256] = "ip_rates.conf";
volatile int running = 1;

unsigned long now_ms() {
    struct timeval tv; 
    gettimeofday(&tv, NULL); 
    return tv.tv_sec*1000 + tv.tv_usec/1000;
}

Client *get_client(const char *ip) {
    pthread_mutex_lock(&lock);
    for (Client *c = clients; c; c=c->next)
        if (!strcmp(c->ip, ip)) { pthread_mutex_unlock(&lock); return c; }
    Client *c = calloc(1,sizeof(Client));
    strcpy(c->ip, ip);
    c->kbps_cfg = 0;
    c->next = clients;
    clients = c;
    pthread_mutex_unlock(&lock);
    return c;
}

void load_ip_rates() {
    FILE *f = fopen(ip_file,"r");
    if(!f){ perror("ip_file"); return; }
    char line[128]; char ip[64]; int kbps;
    while(fgets(line,sizeof(line),f))
        if(sscanf(line,"%63s %d",ip,&kbps)==2){
            Client *c=get_client(ip);
            c->kbps_cfg=kbps;
        }
    fclose(f);
}

int total_kbps_in_use(){
    int total=0;
    pthread_mutex_lock(&lock);
    for(Client*c=clients;c;c=c->next)
        if(c->conns>0)
            total += (c->kbps_cfg>0?c->kbps_cfg:KBPS_DEFAULT);
    pthread_mutex_unlock(&lock);
    return total;
}

void send_err(int sock, int code, const char *msg) {
    char b[128]; int n=sprintf(b,"HTTP/1.1 %d %s\r\nContent-Length:0\r\n\r\n",code,msg);
    send(sock,b,n,0);
}

void rate_send(int sock, int fd, long size, int kbps_per_conn) {
    char buf[BUF]; long sent=0; unsigned long start=now_ms();
    while (1) {
        int r = read(fd, buf, BUF); 
        if (r <= 0) break;
        send(sock, buf, r, 0);
        sent += r;
        if (kbps_per_conn>0) {
            double elapsed = (now_ms()-start)/1000.0;
            double expected = sent / (kbps_per_conn * 125.0);
            if (expected > elapsed) usleep((expected - elapsed)*1e6);
        }
    }
}

void *handle(void *arg) {
    int sock = *(int*)arg; free(arg);
    struct sockaddr_in addr; socklen_t alen = sizeof(addr);
    getpeername(sock, (struct sockaddr*)&addr, &alen);
    char ip[64]; strcpy(ip, inet_ntoa(addr.sin_addr));

    Client *c = get_client(ip);

    pthread_mutex_lock(&lock);
    c->conns++;
    int active = c->conns;
    int cfg = (c->kbps_cfg>0)?c->kbps_cfg:KBPS_DEFAULT;
    int total = total_kbps_in_use();
    pthread_mutex_unlock(&lock);

    if (total > max_kbps) {
        send_err(sock,503,"Service Unavailable");
        close(sock);
        pthread_mutex_lock(&lock); c->conns--; pthread_mutex_unlock(&lock);
        return NULL;
    }

    char req[512]; recv(sock, req, sizeof(req)-1, 0);
    char path[256]; if (sscanf(req,"GET %255s",path)!=1){ send_err(sock,400,"Bad Request"); goto end; }
    if (strstr(path,"..")){ send_err(sock,400,"Invalid Path"); goto end; }

    char full[512]; snprintf(full,sizeof(full),"%s%s",www,path);
    if (full[strlen(full)-1]=='/') strcat(full,"index.html");

    int fd = open(full,O_RDONLY);
    if (fd<0){ send_err(sock,404,"Not Found"); goto end; }

    int is_html = strstr(full,".html")!=NULL;
    char hdr[128]; long len = lseek(fd,0,SEEK_END); lseek(fd,0,SEEK_SET);
    sprintf(hdr,"HTTP/1.1 200 OK\r\nContent-Length:%ld\r\n\r\n",len);
    send(sock,hdr,strlen(hdr),0);

    int kbps_conn = is_html ? 0 : (cfg / (active>0?active:1));
    rate_send(sock,fd,len,kbps_conn);
    close(fd);

    if (is_html) {
        pthread_mutex_lock(&lock);
        c->last_html = now_ms();
        pthread_mutex_unlock(&lock);
    } else {
        pthread_mutex_lock(&lock);
        if (c->last_html>0) {
            c->rtt = now_ms() - c->last_html;
            c->last_html = 0;
        }
        pthread_mutex_unlock(&lock);
    }

end:
    close(sock);
    pthread_mutex_lock(&lock);
    c->conns--;
    pthread_mutex_unlock(&lock);
    return NULL;
}

void *display(void *arg){
    (void)arg;
    while(running){
        sleep(DISPLAY_INTERVAL);
        pthread_mutex_lock(&lock);
        printf("\n=== Clientes Ativos (%lu ms) ===\n", now_ms());
        printf("%-16s %-6s %-8s %-8s\n","IP","Conns","Kbps","RTT(ms)");
        for(Client*c=clients;c;c=c->next)
            if(c->conns>0)
                printf("%-16s %-6d %-8d %-8lu\n",
                    c->ip,c->conns,
                    (c->kbps_cfg>0?c->kbps_cfg:KBPS_DEFAULT),
                    c->rtt);
        printf("Total usado: %d / %d kbps\n", total_kbps_in_use(), max_kbps);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main(int argc,char**argv){
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--port")) port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--www")) strcpy(www,argv[++i]);
        else if(!strcmp(argv[i],"--max-kbps")) max_kbps=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--ip-file")) strcpy(ip_file,argv[++i]);
    }

    load_ip_rates();

    int s = socket(AF_INET,SOCK_STREAM,0);
    int opt=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in srv = {.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=INADDR_ANY};
    bind(s,(struct sockaddr*)&srv,sizeof(srv)); listen(s,10);
    printf("Servidor QoS iniciado na porta %d (max=%d kbps)\n",port,max_kbps);

    pthread_t disp; pthread_create(&disp,NULL,display,NULL);

    while(running){
        int *c = malloc(sizeof(int));
        *c = accept(s,NULL,NULL);
        pthread_t t; pthread_create(&t,NULL,handle,c);
        pthread_detach(t);
    }
    close(s);
    return 0;
}
