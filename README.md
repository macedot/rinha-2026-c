# Rinha de Backend 2026 — C Puro + AVX2 + HIVF

API de detecção de fraude com busca vetorial aproximada (HIVF) implementada em C puro, otimizada com instruções AVX2 e FMA para latência ultra-baixa.

O projeto utiliza um índice de 2 níveis (HIVF) com quantização int16, distância Manhattan e busca adaptativa, servido atrás de HAProxy via Unix Domain Sockets.

## Início Rápido

```bash
docker compose up --build
```

A API fica disponível na porta `9999`.

## API

### `POST /fraud-score`

Recebe os dados de uma transação e retorna se ela deve ser aprovada ou não, junto com o score de fraude.

**Exemplo de requisição:**
```json
{
  "id": "tx-1329056812",
  "transaction": {
    "amount": 41.12,
    "installments": 2,
    "requested_at": "2026-03-11T18:45:53Z"
  },
  "customer": {
    "avg_amount": 82.24,
    "tx_count_24h": 3,
    "known_merchants": ["MERC-003"]
  },
  "merchant": {
    "id": "MERC-016",
    "mcc": "5411",
    "avg_amount": 60.25
  },
  "terminal": {
    "is_online": false,
    "card_present": true,
    "km_from_home": 29.23
  },
  "last_transaction": null
}
```

**Exemplo de resposta:**
```json
{
  "approved": true,
  "fraud_score": 0.0
}
```

## Arquitetura

O sistema é composto por:

- **HAProxy** como load balancer (2 instâncias da API)
- Duas instâncias da API em C, cada uma com 0.45 vCPU e 125MB de memória
- Comunicação via Unix Domain Sockets (tmpfs)
- Índice HIVF de 2 níveis carregado em memória via mmap

```
Cliente
   ↓ HTTP :9999
HAProxy
   ↓ UDS
api1  ←→  api2   (2 instâncias)
```

## Motor de Busca

O núcleo do sistema é um motor de busca vetorial aproximada chamado **HIVF** (Hierarchical Inverted File):

- **2 níveis de clustering**: 256 clusters no nível 1 × 256 clusters no nível 2 = 65.536 clusters no total
- **Quantização int16**: os vetores de 14 dimensões são quantizados para int16 (escala 10000)
- **Distância L2 quadrada** calculada em espaço quantizado
- **Busca em 2 estágios** + probing adaptativo em casos de incerteza
- **K=5 vizinhos** com decisão por contagem simples de fraudes (threshold 0.5)

Todo o cálculo de distância e lower-bound é feito com AVX2 (128-bit e 256-bit) para máxima performance.

## Resultados

Testes realizados com o dataset oficial (54.100 transações):

- **Acurácia**: 0 Falsos Positivos e 0 Falsos Negativos
- **Melhor p99** observado em execuções controladas: **0.669 ms**
- **Média de p99** em 5 execuções completas: abaixo de 1 ms na maioria dos runs
- **Score final**: 6000 pontos (máximo possível)

A implementação prioriza **estabilidade de latência** e **acurácia perfeita**, sacrificando um pouco de latência de pico quando necessário para manter zero erros de classificação.

## Como Executar

### Com Docker (recomendado)

```bash
docker compose up --build
```

### Build local (avançado)

```bash
# 1. Compilar o indexador
make -C indexer

# 2. Gerar o índice
indexer/indexer resources/references.json.gz data

# 3. Compilar o servidor
make

# 4. Executar
INDEX_PATH=data ./rinha-server
```

## Variáveis de Ambiente

| Variável       | Padrão              | Descrição                              |
|----------------|---------------------|----------------------------------------|
| `INDEX_PATH`   | `resources/index.bin` | Diretório contendo os arquivos do índice |
| `UDS_PATH`     | `/tmp/rinha.sock`   | Caminho do socket Unix Domain          |

## Estrutura do Projeto

```
rinha-2026-c/
├── src/
│   ├── main.c
│   ├── knn.c / knn.h          # Motor de busca HIVF + AVX2
│   ├── vectorizer.c / .h      # Extração de features
│   ├── http_server.c / .h
│   └── perf.c / .h
├── indexer/
│   └── indexer.c              # Gera o índice HIVF a partir dos dados
├── resources/
│   ├── references.json.gz
│   ├── normalization.json
│   └── mcc_risk.json
├── docker-compose.yml
├── haproxy.cfg
├── Dockerfile
└── Makefile
```

## Licença

MIT License

---

Desenvolvido para a **Rinha de Backend 2026**.