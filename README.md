<h1 align="center">Rinha de Backend 2026 — C Puro + AVX2</h1>

<p align="center"><strong>API de detecção de fraude com busca vetorial IVF usando kernels AVX2 otimizados à mão — zero dependências, zero overhead de runtime</strong></p>

<p align="center">
  <img src="https://img.shields.io/github/license/macedot/rinha-2026-c?color=blue" alt="License" />
  <img src="https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/AVX2-FMA-4ade80" alt="AVX2+FMA" />
  <img src="https://img.shields.io/badge/Docker-Compose-2496ED?logo=docker&logoColor=white" alt="Docker" />
</p>

---

**Submissão para a [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026).** Implementação em C puro: servidor HTTP com POSIX sockets, vetorizador 14-dim e parser JSON nativos, ponte de busca vetorial IVF/AVX2 via submódulo [`rinha-2026-base`](https://github.com/macedot/rinha-2026-base).

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
                          ┌──────▼────────┐
                          │    passa      │
                          │  cpus: 0.2    │
                          │  mem:  50 MB  │
                          └───┬───────┬───┘
                              │       │
                     UDS /sockets/    UDS /sockets/
                      api1.sock       api2.sock
                   ┌──────▼──────┐ ┌──────▼──────┐
                   │    api1     │ │    api2     │
                   │ cpus: 0.4   │ │ cpus: 0.4   │
                   │ mem: 160 MB │ │ mem: 160 MB │
                   └─────────────┘ └─────────────┘

    ┌──────────────────────────────────────────────────────┐
    │  rinha-c-sockets (tmpfs, 10mb)  ·  rede bridge       │
    │  CPU total: 1.0   |   Memória total: 350 MB          │
    └──────────────────────────────────────────────────────┘
```

### Fluxo da requisição

1. **passa** faz round-robin do payload HTTP sobre **Unix Domain Sockets** — zero overhead de TCP
2. **Servidor POSIX** faz o parse HTTP em buffer de pilha de 32KB — sem alocação no heap
3. **Vetorizador** transforma o JSON em vetor float de 14 dimensões
4. **Ponte IVF** ([`rinha-2026-base`](https://github.com/macedot/rinha-2026-base)) executa busca k-NN aproximada com AVX2: 4096 clusters, busca em dois estágios, varredura AoSoA
5. **fraud_score** = fraudes entre os top 5 / 5; `approved = fraud_score < 0.6`

### Componentes

| Componente | Linguagem | Função |
|-----------|----------|------|
| **passa** | Rust | Balanceador round-robin sobre UDS |
| **Servidor HTTP** | C | Parse HTTP em buffer de pilha, listener UDS |
| **Vetorizador** | C | 14 dimensões seguindo normalização oficial |
| **Parser JSON** | C | Parser customizado sem alocação |
| **Ponte IVF** | C/AVX2 | Submódulo [`rinha-2026-base`](https://github.com/macedot/rinha-2026-base) |
| **Respostas HTTP** | C | Strings pré-computadas |

## Configuração

| Variável | Padrão | Descrição |
|----------|---------|-----------|
| `IVF_NPROBE` | `8` | Clusters sondados na passada rápida |
| `IVF_FULL_NPROBE` | `24` | Clusters sondados na passada completa |
| `CANDIDATES` | `0` | Máximo de blocos por cluster (0 = ilimitado) |
| `INDEX_PATH` | `resources/index.bin` | Caminho do índice IVF |

## Estrutura

```
├── src/
│   ├── main.c              # Entrada: carrega índice, warmup, servidor
│   ├── config.c / .h       # Configuração por variáveis de ambiente
│   ├── http_server.c / .h  # Servidor HTTP com POSIX sockets
│   ├── http_resp.c / .h    # Respostas HTTP pré-computadas
│   └── vectorizer.c / .h   # Vetorizador 14-dim + parser JSON
├── bridge/                 # Submódulo git: macedot/rinha-2026-base
├── data/
│   └── index.bin.gz        # Índice IVF (3M vetores, 4096 clusters)
├── Dockerfile
├── docker-compose.yml
├── Makefile
└── README.md
```

## CI/CD

GitHub Actions publica imagem `ghcr.io/macedot/rinha-2026-c` a cada release.

## Ambiente de Teste

Mac Mini Late 2014 (2.6 GHz Haswell, 8 GB RAM, Ubuntu 24.04). Limites Docker: **1.0 CPU** e **350 MB** de memória total.

## Licença

MIT
