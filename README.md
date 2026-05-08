<h1 align="center">Rinha de Backend 2026 — C Puro + AVX2</h1>

<p align="center"><strong>API de detecção de fraude com busca vetorial IVF usando kernels AVX2 otimizados à mão — zero dependências, zero overhead de runtime</strong></p>

<p align="center">
  <img src="https://img.shields.io/github/license/macedot/rinha-2026-c?color=blue" alt="License" />
  <img src="https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/AVX2-FMA-4ade80" alt="AVX2+FMA" />
  <img src="https://img.shields.io/badge/Docker-Compose-2496ED?logo=docker&logoColor=white" alt="Docker" />
</p>

---

> **Agradecimentos:** O kernel de busca IVF com AVX2 (`bridge.c`) foi originalmente desenvolvido pelo [Jairo Blatt](https://github.com/jairoblatt) no [rinha-2026-rust](https://github.com/jairoblatt/rinha-2026-rust) e adaptado para este projeto em C puro. Muito obrigado pelo excelente trabalho!

**Submissão para a [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026)** — detecção de fraude via busca vetorial. Processa transações de cartão através de um vetorizador de 14 dimensões e busca em 3 milhões de vetores de referência usando IVF/K-means com distância Euclidiana acelerada por intrínsecos AVX2+FMA escritos à mão. C puro, sem dependências, sem alocador — apenas POSIX sockets e SIMD.

## Início Rápido

```bash
docker compose up --build
```

A API escuta na porta `9999`.

### Imagens pré-compiladas (do release do GitHub)

```bash
IMAGE=ghcr.io/macedot/rinha-2026-c:latest docker compose up
```

Substitua `build: .` por `image: ghcr.io/macedot/rinha-2026-c:latest` no `docker-compose.yml`.

## API

### `GET /ready`

Retorna `200 OK` quando o índice foi carregado e a API está pronta para servir.

### `POST /fraud-score`

**Requisição:**
```json
{
  "id": "tx-1329056812",
  "transaction":      { "amount": 41.12, "installments": 2, "requested_at": "2026-03-11T18:45:53Z" },
  "customer":         { "avg_amount": 82.24, "tx_count_24h": 3, "known_merchants": ["MERC-003", "MERC-016"] },
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
                          │  HAProxy 3.3  │
                          │  cpus: 0.2    │
                          │  mem:  30 MB  │
                          └───┬───────┬───┘
                              │       │
                     UDS /sockets/    UDS /sockets/
                      api1.sock       api2.sock
                   ┌──────▼──────┐ ┌──────▼──────┐
                   │    api1     │ │    api2     │
                   │ cpus: 0.4   │ │ cpus: 0.4   │
                   │ mem: 160 MB │ │ mem: 160 MB │
                   │             │ │             │
                   │┌───────────┐│ │┌───────────┐│
                   ││ POSIX     ││ ││ POSIX     ││
                   ││ UDS serv. ││ ││ UDS serv. ││
                   │└─────┬─────┘│ │└─────┬─────┘│
                   │      │      │ │      │      │
                   │┌─────▼─────┐│ │┌─────▼─────┐│
                   ││Vetoriz.   ││ ││Vetoriz.   ││
                   ││ 14 dim.   ││ ││ 14 dim.   ││
                   │└─────┬─────┘│ │└─────┬─────┘│
                   │      │      │ │      │      │
                   │┌─────▼─────┐│ │┌─────▼─────┐│
                   ││ C/AVX2    ││ ││ C/AVX2    ││
                   ││ Busca IVF ││ ││ Busca IVF ││
                   ││ 4096 cls. ││ ││ 4096 cls. ││
                   │└───────────┘│ │└───────────┘│
                   └─────────────┘ └─────────────┘

    ┌──────────────────────────────────────────────────────┐
    │  rinha-sockets (tmpfs, 10mb)  ·  rede bridge         │
    │  CPU total: 1.0   |   Memória total: 350 MB          │
    └──────────────────────────────────────────────────────┘
```

### Fluxo da requisição

1. **Cliente** envia `POST /fraud-score` com JSON da transação para a porta `9999`
2. **HAProxy** faz round-robin do payload HTTP bruto sobre **Unix Domain Sockets** (`/sockets/api1.sock` ou `api2.sock`) — zero overhead de TCP, sem inspeção de payload
3. **Servidor POSIX** faz o parse da requisição HTTP em um buffer de 32KB na pilha — sem alocação no heap
4. **Vetorizador** transforma o payload JSON em um vetor float de 14 dimensões usando as fórmulas oficiais de normalização
5. **Busca IVF C/AVX2** quantiza para `int16`, calcula distâncias dos centroides com AVX2+FMA, seleciona top-N clusters e varre blocos AoSoA com AVX2 FMA + early termination + prefetch. Retorna k=5 vizinhos mais próximos via busca em dois estágios (passada rápida → passada completa para resultados ambíguos)
6. **fraud_score** = fraudes entre os top 5 / 5; `approved = fraud_score < 0.6`

### Componentes

| Componente | Linguagem | Função |
|-----------|----------|------|
| **HAProxy 3.3** | C | Balanceador de carga layer 7, round-robin sobre UDS |
| **Servidor POSIX** | C | Parse HTTP em buffer de pilha, listener UDS |
| **Vetorizador** | C | Vetorizador de features 14-dim seguindo regras oficiais de normalização |
| **Parser JSON** | C | Parser JSON customizado sem alocação |
| **Busca IVF** | C/AVX2 | Busca IVF/K-means: 4096 clusters, distância de centroides AVX2 com FMA, seleção top-N com AVX2, varredura de blocos AoSoA com AVX2 + early termination + prefetch, busca adaptativa em dois estágios |
| **Respostas HTTP** | C | Strings de resposta HTTP pré-computadas |

### Transporte

O HAProxy se comunica com as instâncias da API via **Unix Domain Sockets** em um volume `tmpfs` (`rinha-sockets`). Isso elimina completamente o overhead de TCP — sem pilha de rede do kernel, sem buffers de socket, sem filas de accept. Um único volume tmpfs de 10 MB comporta ambos os arquivos de socket da API.

### Stack Tecnológico

- **C11** — POSIX sockets, parser JSON sem alocação, servidor HTTP com buffer de pilha
- **AVX2+FMA** — intrínsecos otimizados à mão para distância de centroides, seleção top-N, varredura de blocos AoSoA com prefetch e early termination
- **HAProxy 3.3** — balanceador de carga stateless round-robin
- **Docker Compose** — 3 serviços, rede bridge, limites de recursos via `deploy.resources.limits`

## Destaques de Otimização

O kernel de busca IVF passou por micro-otimizações extensivas visando latência p99 em um [Mac Mini Late 2014](https://support.apple.com/en-us/111931) (2.6 GHz Haswell, 8 GB RAM) com limites Docker de 1.0 CPU e 350 MB de memória total.

| Otimização | Técnica |
|------------|---------|
| **Distância de centroides AVX2** | Distância vetorizada: 16 centroides/iter com acumulação FMA, centroides transpostos para leituras contíguas de dimensão |
| **Seleção top-N AVX2** | Seleção de clusters baseada em máscara com comparações de 8 vias |
| **Varredura de blocos AoSoA** | Blocos de 8 vetores em layout column-major, processamento de pares de dimensão com FMA, early termination após 4/7 pares, prefetch por software |
| **Busca em dois estágios** | Passada rápida com nprobe=8, passada completa com nprobe=24 apenas para resultados ambíguos (2-3 fraudes) |
| **Reordenação de clusters** | Varre primeiro os clusters menores para apertar a pior distância mais cedo |
| **Centroides transpostos** | Layout column-major dos centroides para cargas AVX2 cache-friendly |
| **Zero alocação** | Todo parsing em buffers de pilha, respostas HTTP pré-computadas |
| **Transporte UDS** | HAProxy ↔ API via Unix domain sockets (zero overhead de TCP) |

## Configuração

| Variável | Padrão | Descrição |
|----------|---------|-----------|
| `IVF_NPROBE` | `8` | Clusters sondados na passada rápida |
| `IVF_FULL_NPROBE` | `24` | Clusters sondados na passada completa (resultados ambíguos) |
| `CANDIDATES` | `0` | Máximo de candidatos a varrer (0 = ilimitado) |
| `INDEX_PATH` | `resources/index.bin` | Caminho para o arquivo de índice IVF |

Todas as constantes de normalização seguem o `normalization.json` oficial.

## Estrutura do Repositório

```
├── src/
│   ├── main.c              # Ponto de entrada: carrega índice, warmup, executa servidor
│   ├── config.c / .h       # Configuração por variáveis de ambiente
│   ├── http_server.c / .h  # Servidor HTTP com POSIX sockets (TCP + UDS)
│   ├── http_resp.c / .h    # Respostas HTTP pré-computadas
│   ├── vectorizer.c / .h   # Vetorizador de features 14-dim + parser JSON
│   └── bridge.c / .h       # Kernel de busca IVF C/AVX2 (centroides + top-N + varredura AoSoA)
├── resources/
│   ├── mcc_risk.json       # Tabela de risco por MCC
│   ├── references.json.gz  # 3M vetores de referência rotulados
│   ├── example-payloads.json
│   └── example-references.json
├── data/
│   └── index.bin.gz        # Índice IVF pré-construído (3M vetores, 4096 clusters, ~30MB comprimido)
├── Dockerfile              # Multi-estágio: build gcc → runtime Debian slim
├── docker-compose.yml      # Deploy de 3 serviços com limites de recursos
├── haproxy.cfg             # Configuração de UDS round-robin do HAProxy
├── Makefile                # Build: gcc -O3 -mavx2 -mfma
├── .github/workflows/release.yml  # CI: build & push da imagem Docker para GHCR
├── LICENSE                 # MIT
├── info.json               # Dados do participante da Rinha
└── README.md
```

> O branch `submission` contém apenas `docker-compose.yml`, `haproxy.cfg` e `info.json` — sem código fonte. Ele referencia a imagem pré-compilada `ghcr.io/macedot/rinha-2026-c:latest`.

## CI/CD

GitHub Actions compila e publica uma imagem Docker `linux/amd64` em `ghcr.io/macedot/rinha-2026-c` a cada release publicado (excluindo pre-releases). As imagens são tagueadas com a versão do release e `latest`.

## Ambiente de Teste

O teste oficial executa em um Mac Mini Late 2014 (2.6 GHz Haswell, 8 GB RAM, Ubuntu 24.04) com limites Docker de **1.0 CPU** e **350 MB de memória** entre todos os serviços. Todas as otimizações foram ajustadas especificamente para este hardware.

## Licença

Este projeto está licenciado sob a [Licença MIT](LICENSE).
