Servidor HTTP com QoS (C + Pthreads)
Este projeto implementa um servidor HTTP concorrente em C, utilizando pthreads, com suporte a QoS por IP e controle de admissão.O código foi desenvolvido como parte do trabalho prático da disciplina Fundamentos e Avaliação de Redes de Computadores (2025/2).

Funcionalidades:
Atende múltiplas conexões simultâneas usando threads.
Implementa controle de taxa por IP:

● Arquivo auxiliar ("ip_rates.conf") define limites de kbps para cada IP.
● Caso o IP não esteja configurado, aplica 1000 kbps por padrão.
● Se houver múltiplas conexões do mesmo IP, a taxa é dividida igualmente entre as conexões.

Controle de admissão:
● O servidor respeita um limite máximo de vazão ("--max-kbps").
● Se a soma das taxas configuradas ultrapassar esse limite, novas conexões recebem "503 Service Unavailable".

Estimativa de RTT:
● Calculada a partir do tempo entre a requisição do arquivo HTML e a requisição do primeiro objeto subsequente.
● Exibição em tempo real (console):
● Lista os clientes ativos, conexões, taxa configurada e RTT estimado.
● Diretório de conteúdo configurável ("--www").
● Testes recomendados com "wget" ou "curl" (navegadores podem não requisitar automaticamente objetos de teste).

Estrutura do Projeto:
/http_qos_server
│── server.c # Código-fonte principal
│── ip_rates.conf # Configuração de taxas por IP
│── www/ # Diretório com arquivos servidos│
├── index.html│
├── img1.jpg (10 MB)
├── img2.jpg (10 MB)
└── img3.jpg (10 MB)
└── README.md 

Compilação:
Requer Linux com suporte a POSIX threads (pthreads).

Compilar com:
gcc -pthread -o http_qos_server server.c
Execução:

Exemplo de execução:
./http_qos_server --port 8080 --max-kbps 10000 --ip-file ip_rates.conf --www ./www

Parâmetros
"--port PORT" → Porta do servidor (padrão: "8080")
"--max-kbps N" → Vazão máxima do servidor em kbps (padrão: "10000")
"--ip-file path" → Arquivo de configuração de IPs e limites de taxa
"--www path" → Diretório raiz dos arquivos servidos (padrão: "./www")
Arquivo "ip_rates.conf"
Define limites de taxa por IP (em kbps).

Formato:
IP LIMITE_KBPS

Exemplo:
192.168.0.10 2000192.168.0.11 500
- Se o IP não estiver listado, aplica-se 1000 kbps.- Todas as conexões de um mesmo IP compartilham a taxa configurada.
---
Testes:
1. Criar diretório "www/" com:
- "index.html" contendo referências a várias imagens grandes (ex.: "img1.jpg", "img2.jpg" com vários MB cada).
2. Usar "wget" ou "curl" para simular acessos:
wget -r -np -nH -P ./downloads http://127.0.0.1:8080/index.html
3. Verificar saída no console:
- Clientes atendidos - Conexões ativas - Largura de banda configurada - RTT estimado
4. Monitorar tráfego com Wireshark ou tcpdump:
sudo tcpdump -i lo port 8080

Relatórios e Avaliação:
● Avaliar desempenho comparando cenários Ethernet vs Wi-Fi.
● Usar ferramentas como Wireshark e Iptraf para medir vazão, atraso e concorrência.
● Incluir análises nos relatórios das versões do trabalho.

Autores:
● João Duarte
● Renan Antunes
Este projeto foi desenvolvido integralmente pela equipe, sem ajuda não autorizada de terceiros.Trechos de código externos foram devidamente referenciados quando aplicável.
Licença:
Uso acadêmico exclusivo para a disciplina Fundamentos e Avaliação de Redes de Computadores (2025/2).
