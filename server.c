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

// CÓDIGO CORRIGIDO
int total_kbps_in_use(){
    int total=0;
    // O lock NÃO é mais pego aqui
    for(Client*c=clients;c;c=c->next)
        if(c->conns>0)
            total += (c->kbps_cfg>0?c->kbps_cfg:KBPS_DEFAULT);
    // O unlock NÃO é mais feito aqui
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

// VERSÃO CORRIGIDA PARA HTTP 1.1
void *handle(void *arg) {
    int sock = *(int*)arg; free(arg);
    struct sockaddr_in addr; socklen_t alen = sizeof(addr);
    getpeername(sock, (struct sockaddr*)&addr, &alen);
    char ip[64]; strcpy(ip, inet_ntoa(addr.sin_addr));

    Client *c = get_client(ip);

    // --- Início: Controle de Admissão (feito 1 vez por conexão) ---
    pthread_mutex_lock(&lock);
    c->conns++;
    int active = c->conns;
    int cfg = (c->kbps_cfg>0)?c->kbps_cfg:KBPS_DEFAULT;
    int total = total_kbps_in_use();
    pthread_mutex_unlock(&lock);

    if (total > max_kbps) {
        send_err(sock,503,"Service Unavailable");
        goto end; // Rejeita a conexão, pula para o fim
    }
    // --- Fim: Controle de Admissão ---

    // --- Início: Definir Timeout do Socket ---
    // Define um timeout de 15 segundos. Se o cliente não enviar
    // uma nova requisição nesse tempo, recv() falhará.
    struct timeval tv;
    tv.tv_sec = 15; // 15 segundos
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    // --- Fim: Timeout ---


    // --- Início: Loop Principal de Requisições (HTTP 1.1) ---
    while(running) {
        char req[512];
        int r = recv(sock, req, sizeof(req)-1, 0);

        // Se recv() retornar 0 ou menos, o cliente desconectou ou deu timeout
        if (r <= 0) {
            break; // Sai do loop while
        }
        req[r] = 0; // Garante que a string termina

        // Verifica se o cliente pediu para fechar a conexão
        int keep_alive = 1;
        if (strstr(req, "Connection: close")) {
            keep_alive = 0;
        }

        char path[256]; 
        if (sscanf(req,"GET %255s",path)!=1){ 
            send_err(sock,400,"Bad Request");
            continue; // Erro, mas continua no loop esperando próxima req
        }
        if (strstr(path,"..")){ 
            send_err(sock,400,"Invalid Path"); 
            continue; // Erro, mas continua no loop
        }

        char full[512]; snprintf(full,sizeof(full),"%s%s",www,path);
        if (full[strlen(full)-1]=='/') strcat(full,"index.html");

        int fd = open(full,O_RDONLY);
        if (fd<0){ 
            send_err(sock,404,"Not Found"); 
            continue; // Erro, mas continua no loop
        }

        int is_html = strstr(full,".html")!=NULL;

        // Atualiza o número de conexões ativas *deste* IP
        // para dividir a banda corretamente 
        pthread_mutex_lock(&lock);
        active = c->conns; 
        pthread_mutex_unlock(&lock);
        
        char hdr[256]; // Buffer maior para o novo cabeçalho
        long len = lseek(fd,0,SEEK_END); lseek(fd,0,SEEK_SET);
        
        // Envia o cabeçalho HTTP 1.1 correto
        sprintf(hdr,"HTTP/1.1 200 OK\r\nContent-Length:%ld\r\nConnection: %s\r\n\r\n",
                len,
                keep_alive ? "keep-alive" : "close");
        send(sock,hdr,strlen(hdr),0);

        // Envia o arquivo com controle de taxa [cite: 35]
        int kbps_conn = is_html ? 0 : (cfg / (active>0?active:1));
        rate_send(sock,fd,len,kbps_conn);
        close(fd);

        // --- Início: Lógica RTT [cite: 37] ---
        if (is_html) {
            pthread_mutex_lock(&lock);
            c->last_html = now_ms();
            pthread_mutex_unlock(&lock);
        } else {
            pthread_mutex_lock(&lock);
            if (c->last_html>0) {
                c->rtt = now_ms() - c->last_html;
                c->last_html = 0; // Reseta para a próxima medição
            }
            pthread_mutex_unlock(&lock);
        }
        // --- Fim: Lógica RTT ---

        // Se o cliente pediu para fechar, saímos do loop
        if (!keep_alive) {
            break;
        }

    } // --- Fim: Loop Principal de Requisições ---

end:
    close(sock); // O socket só é fechado aqui, no fim da thread

    // Libera o "slot" de conexão
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
