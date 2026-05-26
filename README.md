<h1 align="center">Rinha de Backend 2026 — C Puro + AVX2 + HIVF</h1>

<p align="center"><strong>API de detecção de fraude com busca vetorial HIVF usando kernels AVX2 otimizados à mão — zero dependências, zero overhead de runtime</strong></p>

<p align="center">
  <img src="https://img.shields.io/github/license/macedot/rinha-2026-c?color=blue" alt="License" />
  <img src="https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/AVX2-FMA-4ade80" alt="AVX2+FMA" />
  <img src="https://img.shields.io/badge/Docker-Compose-2496ED?logo=docker&logoColor=white" alt="Docker" />
  <img src="https://img.shields.io/badge/LB-HAProxy-0094C2?logo=haproxy&logoColor=white" alt="HAProxy" />
</p>

---

**Submissão para a [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026).** Escrita em C puro, otimizada para latência ultra-baixa usando um motor de busca **HIVF K-Means** de 2 níveis com distância Manhattan e AVX2, servida atrás de **HAProxy** sem nenhum `security_opt`.

## Agradecimentos

Este projeto não existiria sem a inspirationação e groundwork de projetos anteriores na comunidade:

- **[Rinha de Backend 2026 — Rust (jaoppb)](https://github.com/jaoppb/rinha-de-backend-2026-rust)** — A abordagem de features (sin/cos, log transforms, weighted scoring) e a baseline de resultados que inspirou diversas otimizações. Thank you for sharing your work with the community!
- **Jairo Blatti** — O kernel de busca IVF com AVX2 original que serviu de inspiração inicial, re-arquitetado e incorporado nativamente.

## Início Rápido

```bash
docker compose up --build
```

A API escuta na porta `9999`.

## API

### `GET /ready`

Retorna `200 OK` quando o índice foi carregado e a API está pronta.

### `POST /fraud-score`

**Requisição:**
```json
{
  "id": "tx-1329056812",
  "transaction":      { "amount": 41.12, "installments": 2, "requested_at": "2026-03-11T18:45:53Z" },
  "customer":         { "avg_amount": 82.24, "tx_count_24h": 3, "known_merchants": ["MERC-003"] },
  "merchant":         { "id": "MERC-016", "mcc": "5411", "avg_amount": 60.25 },
  "terminal":         { "is_online": false, "card_present": true, "km_from_home": 29.23 },
  "last_transaction": null
}
```

**Resposta:**
```json
{ "approved": true, "fraud_score": 0.0 }
```

## Arquitetura

```
                           ┌──────────┐
                           │  Cliente │
                           └─────┬────┘
                                 │ HTTP :9999
                          ┌──────▼──────────┐
                          │   HAProxy       │
                          │ (UDS backends)  │
                          │ cpus: 0.2       │
                          │ mem:  30 MB     │
                          └───┬───────┬─────┘
                              │       │
                 UDS /run/sock/  UDS /run/sock/
                  api1.sock      api2.sock
               ┌──────▼──────┐ ┌──────▼──────┐
               │    api1     │ │    api2     │
               │ cpus: 0.45  │ │ cpus: 0.45  │
               │ mem: 125 MB │ │ mem: 125 MB │
               └─────────────┘ └─────────────┘

   ┌──────────────────────────────────────────────────────┐
   │  rinha-c-sock (tmpfs, 10mb)  ·  rede bridge          │
   │  CPU total: 1.0   |   Memória total: 280 MB          │
   └──────────────────────────────────────────────────────┘
```

### Fluxo da requisição

1. **HAProxy** recebe conexões TCP na porta 9999 e encaminha via sockets Unix Domain para uma das duas instâncias da API.
2. O servidor C aceita a conexão no UDS, faz parse HTTP e vetoriza o payload.
3. **Motor KNN Nativo (AVX2)** executa a busca vetorial aproximada em 3 estágios.
4. **fraud_score** = soma ponderada de fraudes no K=7 / K; `approved = fraud_score < 0.44`.

Não é mais utilizado `security_opt: seccomp:unconfined` em nenhum serviço.

> **Nota sobre arquivos removidos**: Em versões anteriores existiam `lb.c` (load balancer customizado), `test/` (testes unitários), `autoresearch.sh` (script de benchmark local) e arquivos de exemplo (`example-payloads.json`, `example-references.json`). Foram removidos para manter o repositório enxuto — apenas o código essencial permanece.

## Motor de Busca (o mais importante)

- **HIVF com 2 níveis**: 256 clusters L1 × 256 clusters L2 = **65.536 clusters total**
- **16 dimensões**: Features com sin/cos para cyclical (hour, day), log transforms, e binary packing
- **Distância Manhattan (L1)** em vez de Euclidiana
- **K=7 com scoring ponderado exponencial**: `weight = exp(-dist × 0.5)`
- **Busca em 3 estágios**: L1(256→16) → L2(4096→256) → Records
- **mmap zero-copy** para carregamento do índice
- **Probing adaptativo**: quando a pontuação está na zona de incerteza (0.38–0.50), busca clusters L2 adicionais
- Tudo implementado manualmente em AVX2 + FMA, sem bibliotecas externas.

## Resultados do Benchmark

Teste oficial completo (54.100 requisições):

| Métrica | Antes | Depois |
|---------|-------|--------|
| **p99 latência** | 0.26 ms | 0.77 ms |
| **Falsos Positivos** | 22 | **0** |
| **Falsos Negativos** | 1 | **0** |
| **final_score** | 5575.51 | **6000.00** |

**Acurácia perfeita**: 0 FP, 0 FN, score máximo de 6000 pontos.

O p99 aumentou debido à busca em mais clusters (256 L2 vs antigo 5-24), mas a melhoria em acurácia é dramática — de 5575 para 6000 pontos.

## Configuração

Principais variáveis de ambiente:

| Variável            | Padrão | Descrição                              |
|---------------------|--------|----------------------------------------|
| `INDEX_PATH`        | `/app/data` | Caminho do diretório do índice binário |
| `IVF_NPROBE`        | 5      | Clusters L1 para busca rápida          |
| `IVF_FULL_NPROBE`   | 24     | Clusters L2 no fallback                |
| `UDS_PATH`          | `/tmp/rinha.sock` | Path do socket Unix Domain     |

## Estrutura

```
├── src/
│   ├── main.c            # Ponto de entrada
│   ├── config.c/h        # Configuração por variáveis de ambiente
│   ├── http_server.c/h   # Servidor HTTP sobre Unix Domain Socket
│   ├── http_resp.c/h     # Formatação de respostas JSON
│   ├── knn.c/h           # Motor de busca HIVF com AVX2
│   ├── vectorizer.c/h    # Extração de features 16-dim
│   ├── scm_rights.c/h    # Controle de socket Unix Domain
│   └── perf.c/h          # Medição de latência
├── indexer/
│   ├── indexer.c         # Indexador HIVF 2-level K-Means
│   └── Makefile
├── resources/
│   ├── references.json.gz    # Base de dados de referência (3M registros)
│   ├── normalization.json    # Pesos de features
│   └── mcc_risk.json         # Tabela de risco por MCC
├── docker-compose.yml
├── haproxy.cfg
├── Dockerfile
└── Makefile
```

## Build Local

```bash
# Compilação e execução completa via Docker
docker compose up --build
```

O `Dockerfile` compila tudo automaticamente: o indexador gera o índice HIVF a partir de `resources/references.json.gz` (3M registros) e o servidor C é compilado com `-O3 -march=haswell -flto -static`.

Para build manual:
```bash
make indexer/indexer        # Compila o indexador
indexer/indexer resources/references.json.gz data   # Gera o índice
make                        # Compila o servidor
./rinha-server              # Executa
```

## Licença

MIT