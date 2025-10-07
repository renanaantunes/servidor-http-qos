#!/bin/bash
# ============================================================
# test_qos.sh - Script de teste automatizado para Servidor HTTP com QoS
# ============================================================
# Autor: Equipe do Trabalho (Fundamentos e Avaliação de Redes de Computadores)
# Descrição:
#   Simula múltiplos clientes acessando o servidor simultaneamente.
#   Mede tempo total de download e calcula vazão média (kbps).
# ============================================================

# -------- CONFIGURAÇÕES --------
SERVER_IP="127.0.0.1"
SERVER_PORT="8080"
URL="http://${SERVER_IP}:${SERVER_PORT}/index.html"
CLIENTS=5              # número de clientes simultâneos
OUT_DIR="downloads_test"
LOG_FILE="relatorio_teste.csv"
REPETICOES=3           # número de execuções para cada cenário

# -------- LIMPAR RESULTADOS ANTERIORES --------
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
echo "Teste iniciado em $(date)"
echo "CLIENTES_SIMULTANEOS,TEMPO_TOTAL_SEGUNDOS,VAZAO_MEDIA_KBPS" > "$LOG_FILE"

# -------- FUNÇÃO DE TESTE --------
executar_teste() {
    local n_clients=$1
    echo ""
    echo ">>> Executando teste com $n_clients cliente(s)..."

    # Marca tempo inicial
    local start_time=$(date +%s)

    # Executa múltiplos clientes simultaneamente com wget
    for i in $(seq 1 "$n_clients"); do
        mkdir -p "$OUT_DIR/client_$i"
        wget -q -r -np -nH -P "$OUT_DIR/client_$i" "$URL" &
    done

    # Aguarda todos os downloads terminarem
    wait

    # Marca tempo final
    local end_time=$(date +%s)
    local total_time=$((end_time - start_time))

    # Calcula tamanho total baixado (bytes → kilobits)
    local total_bytes=$(du -cb "$OUT_DIR" | grep total | awk '{print $1}')
    local total_kbits=$(( (total_bytes * 8) / 1000 ))

    # Calcula vazão média (kbps)
    local vazao=$(( total_kbits / total_time ))

    # Salva no CSV
    echo "${n_clients},${total_time},${vazao}" >> "$LOG_FILE"

    echo "Tempo total: ${total_time}s | Vazão média: ${vazao} kbps"
}

# -------- EXECUTAR CENÁRIOS --------
for r in $(seq 1 "$REPETICOES"); do
    echo ""
    echo "===== REPETIÇÃO $r ====="
    for c in 1 2 3 5 10; do
        executar_teste "$c"
        sleep 3
    done
done

# -------- FINALIZAÇÃO --------
echo ""
echo "--------------------------------------------"
echo "Testes concluídos!"
echo "Relatório salvo em: $LOG_FILE"
echo "--------------------------------------------"
