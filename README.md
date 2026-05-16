<h1 align="center">Rinha de Backend 2026 — C Puro + AVX2</h1>

<p align="center"><strong>API de detecção de fraude com busca vetorial IVF usando kernels AVX2 otimizados à mão — zero dependências, zero overhead de runtime</strong></p>

<p align="center">
  <img src="https://img.shields.io/github/license/macedot/rinha-2026-c?color=blue" alt="License" />
  <img src="https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/AVX2-FMA-4ade80" alt="AVX2+FMA" />
  <img src="https://img.shields.io/badge/Docker-Compose-2496ED?logo=docker&logoColor=white" alt="Docker" />
</p>

---

**Submissão para a [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026).** Implementação em C puro: servidor HTTP epoll, vetorizador 14-dim e parser JSON inline, ponte de busca vetorial IVF/AVX2 via submódulo [`rinha-2026-base`](https://github.com/macedot/rinha-2026-base).

> **Agradecimentos:** O kernel de busca IVF com AVX2 foi originalmente desenvolvido por [Jairo Blatt](https://github.com/jairoblatt) e adaptado neste projeto. Veja [`rinha-2026-base`](https://github.com/macedot/rinha-2026-base) para detalhes do algoritmo.

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
                          │ so-no-forevis   │
                          │ v1.0.0          │
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

1. **so-no-forevis** aceita conexões TCP e passa file descriptors via **SCM_RIGHTS** ao servidor C sobre Unix Domain Sockets. O servidor C trata o HTTP diretamente no socket TCP recebido.
2. **Servidor epoll** aceita conexões UDS (e FDs passados via SCM_RIGHTS), faz parse HTTP em buffer de pilha de 32KB
3. **Vetorizador inline** transforma o JSON em vetor float de 14 dimensões (parser customizado, sem `strtof`)
4. **Ponte IVF** ([`rinha-2026-base`](https://github.com/macedot/rinha-2026-base)) executa busca k-NN aproximada com AVX2: 4096 clusters, busca em dois estágios, varredura AoSoA
5. **fraud_score** = fraudes entre os top 5 / 5; `approved = fraud_score < 0.6`

### Componentes

| Componente | Linguagem | Função |
|-----------|----------|--------|
| **so-no-forevis** | Rust | Balanceador round-robin, aceita TCP, passa FDs via SCM_RIGHTS |
| **SCM_RIGHTS** | C | Recebe file descriptors TCP do LB via recvmsg() |
| **Servidor HTTP** | C | Epoll event-driven, listener UDS, keep-alive |
| **Vetorizador** | C | 14 dimensões, parser JSON inline sem alocação |
| **Parser float** | C | `parse_f32()` customizado (sem `strtof`) |
| **Ponte IVF** | C/AVX2 | Submódulo [`rinha-2026-base`](https://github.com/macedot/rinha-2026-base) |
| **Respostas HTTP** | C | Strings pré-computadas, tamanhos em lookup table |

## Otimizações de Performance

Score perfeito 6000 (p99=0.23ms, 0 falsos positivos, 0 falsos negativos, 54.100 requisições):

| Otimização | Impacto |
|-----------|---------|
| Servidor epoll (vs blocking accept-per-conn) | ~20% |
| Parser float inline (vs `strtof`) | ~5% |
| `-march=haswell -mtune=haswell` | ~5% |
| Respostas pré-computadas (sem `strlen`) | ~2% |
| Alinhamento cache-line da pool de conexões | ~5% |
| Remoção de timing de debug da ponte IVF | ~1% |
| Lookup tables para valores discretos | ~2% |
| SCM_RIGHTS fd passing (vs proxy) | ~15% |

**Arquitetura de rede:** UDS em tmpfs via so-no-forevis v1.0.0 com SCM_RIGHTS. TCP foi removido — UDS + fd passing é o caminho.

## Configuração

| Variável | Padrão | Descrição |
|----------|---------|-----------|
| `IVF_NPROBE` | `8` | Clusters sondados na passada rápida |
| `IVF_FULL_NPROBE` | `24` | Clusters sondados na passada completa |
| `CANDIDATES` | `0` | Máximo de blocos por cluster (0 = ilimitado) |
| `INDEX_PATH` | `resources/index.bin` | Caminho do índice IVF |
| `UDS_PATH` | `/tmp/rinha.sock` | Caminho do socket Unix (fallback: `SOCKET_PATH`) |
| `UDS_MODE` | `666` | Permissões do socket (octal) |
| `UNLINK_UDS` | `1` | Remove socket existente antes de bind |

## Estrutura

```
├── src/
│   ├── main.c              # Entrada: carrega índice, warmup, servidor
│   ├── config.c / .h       # Configuração por variáveis de ambiente
│   ├── http_server.c / .h  # Servidor HTTP epoll + UDS
│   ├── http_resp.c / .h    # Respostas HTTP pré-computadas
│   ├── scm_rights.c / .h   # Recebe FDs TCP via SCM_RIGHTS
│   ├── vectorizer.c / .h   # Vetorizador 14-dim + parser JSON inline
│   └── knn.c / .h          # Motor de busca KNN (IVF1) com AVX2
├── resources/
│   └── index.bin.gz        # Índice IVF (3M vetores, 4096 clusters)
├── Dockerfile
├── docker-compose.yml
├── Makefile
└── README.md
```

## Build

```bash
make clean && make
```

Flags do compilador (`-O3 -march=haswell -mtune=haswell -flto -static`) otimizadas para CPUs Intel com AVX2.

## CI/CD

GitHub Actions publica imagem `ghcr.io/macedot/rinha-2026-c` a cada release.

## Ambiente de Teste

Mac Mini Late 2014 (2.6 GHz Haswell, 8 GB RAM, Ubuntu 24.04). Limites Docker: **1.0 CPU** e **330 MB** de memória total.

## Licença

MIT
