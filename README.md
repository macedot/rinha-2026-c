<h1 align="center">Rinha de Backend 2026 — C Puro + AVX2 + HAProxy</h1>

<p align="center"><strong>API de detecção de fraude com busca vetorial IVF usando kernels AVX2 otimizados à mão — zero dependências, zero overhead de runtime</strong></p>

<p align="center">
  <img src="https://img.shields.io/github/license/macedot/rinha-2026-c?color=blue" alt="License" />
  <img src="https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/AVX2-FMA-4ade80" alt="AVX2+FMA" />
  <img src="https://img.shields.io/badge/Docker-Compose-2496ED?logo=docker&logoColor=white" alt="Docker" />
  <img src="https://img.shields.io/badge/LB-HAProxy-0094C2?logo=haproxy&logoColor=white" alt="HAProxy" />
</p>

---

**Submissão para a [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026).** Escrita em C puro, otimizada para latência ultra-baixa usando um motor de busca **IVF K-Means** com aritmética `int16_t` nativa e AVX2, servida atrás de **HAProxy** sem nenhum `security_opt`.

> **Agradecimentos:** O kernel de busca IVF com AVX2 original de Jairo Blatt serviu de inspiração inicial, re-arquitetado e incorporado nativamente.

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
                   │ cpus: 0.4   │ │ cpus: 0.4   │
                   │ mem: 150 MB │ │ mem: 150 MB │
                   └─────────────┘ └─────────────┘

    ┌──────────────────────────────────────────────────────┐
    │  rinha-c-sock (tmpfs, 10mb)  ·  rede bridge          │
    │  CPU total: 1.0   |   Memória total: 330 MB          │
    └──────────────────────────────────────────────────────┘
```

### Fluxo da requisição

1. **HAProxy** recebe conexões TCP na porta 9999 e encaminha via sockets Unix Domain para uma das duas instâncias da API.
2. O servidor C aceita a conexão no UDS, faz parse HTTP e vetoriza o payload.
3. **Motor KNN Nativo (AVX2)** executa a busca vetorial aproximada.
4. **fraud_score** = fraudes entre os top 5 / 5; `approved = fraud_score < 0.6`.

Não é mais utilizado `security_opt: seccomp:unconfined` em nenhum serviço.

## Motor de Busca (o mais importante)

- **IVF com 4096 clusters** (K-Means pré-computado).
- Vetores quantizados para `int16_t` (escala 10000).
- Layout **AoSoA** (8 vetores por bloco) extremamente amigável ao cache e SIMD.
- **Early termination** agressivo: após apenas 8 das 14 dimensões, muitos blocos são descartados.
- **Busca em dois estágios**: nprobe=5 na maioria dos casos. Quando o top-5 retorna exatamente 2 ou 3 fraudes (zona de incerteza), faz fallback automático para nprobe=24.
- Tudo implementado manualmente em AVX2 + FMA, sem bibliotecas externas.

## Resultados do Benchmark

Teste oficial completo (54.100 requisições):

- **p99**: 0.26 ms
- **final_score**: 5575.51
- Falsos positivos: 22
- Falsos negativos: 1
- Erros HTTP: 0

## Configuração

Principais variáveis de ambiente:

| Variável            | Padrão | Descrição                              |
|---------------------|--------|----------------------------------------|
| `IVF_NPROBE`        | 5      | Clusters na busca rápida               |
| `IVF_FULL_NPROBE`   | 24     | Clusters no fallback                   |
| `INDEX_PATH`        | ...    | Caminho do índice binário              |

## Estrutura

```
├── src/
│   ├── main.c, config.*, http_server.*, knn.c (AVX2), vectorizer.c ...
├── resources/index.bin.gz
├── haproxy.cfg
├── docker-compose.yml
├── Dockerfile
└── Makefile
```

## Build Local

```bash
make clean && make
```

Compilado com `-O3 -march=haswell -mtune=haswell -flto -static`.

## Licença

MIT
