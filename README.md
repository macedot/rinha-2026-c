<h1 align="center">Rinha de Backend 2026 — C Puro + AVX2</h1>

<p align="center"><strong>API de detecção de fraude com busca vetorial IVF usando kernels AVX2 otimizados à mão — zero dependências, zero overhead de runtime</strong></p>

<p align="center">
  <img src="https://img.shields.io/github/license/macedot/rinha-2026-c?color=blue" alt="License" />
  <img src="https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/AVX2-FMA-4ade80" alt="AVX2+FMA" />
  <img src="https://img.shields.io/badge/Docker-Compose-2496ED?logo=docker&logoColor=white" alt="Docker" />
</p>

---

**Submissão para a [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026).** Escrita em C puro, otimizada para latência ultra-baixa usando multiplexação **io_uring**, vetorização **AVX2**, e um motor de busca **IVF K-Means** com aritmética `int16_t` nativa.

> **Agradecimentos:** O kernel de busca IVF com AVX2 original de Jairo Blatt serviu de inspiração inicial, re-arquitetado e incorporado nativamente na base principal.

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
                          │ haproxy         │
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

1. **HAProxy** load balances to the API instances over Unix Domain Sockets. The C servers accept connections on their UDS sockets (and can still receive raw FDs via the legacy SCM_RIGHTS path if needed). The C server handles HTTP directly on the received sockets.
2. **Servidor io_uring** aceita as requisições assincronamente e faz parse HTTP através de buffers de stack pré-alocados.
3. **Vetorizador inline** transforma o JSON em vetor float de 14 dimensões (parser customizado, sem dependências padrão da `libc`).
4. **Motor KNN Nativo** executa a busca vetorial aproximada com AVX2 no espaço de inteiros 16-bit.
5. **fraud_score** = fraudes entre os top 5 / 5; `approved = fraud_score < 0.6`

## 🚀 Otimizações Extremas

Resultados do benchmark (`p99 < 0.25ms`, 0 falsos positivos, 0 falsos negativos, 54.100 requisições simultâneas).

### 1. Motor de Busca KNN (AVX2 nativo 16-bit)
A busca k-NN sobre **3 milhões de vetores de referência** (14-dims) roda em sub-milisegundos usando um índice Inverted File (IVF1) com K-Means.

*   **Quantização `int16_t`**: Os vetores floats são normalizados entre `[-1.0, 1.0]` e multiplicados por `10000`, sendo armazenados como inteiros de 16 bits. Isso reduz a memória à metade (~84MB) e melhora massivamente a densidade do cache L1/L2.
*   **Layout AoSoA (Array of Structures of Arrays)**: Os blocos armazenam 8 vetores transpostos por dimensão (`dim0_v0..v7`, `dim1_v0..v7`). O processador carrega 8 dimensões idênticas paralelamente, mitigando gargalos comuns de *scatter/gather*.
*   **Matemática Nativa `i16`**: A distância L2 Euclidiana ocorre quase inteiramente no espaço de inteiros. As subtrações rodam na instrução `_mm_sub_epi16`, convertidas pra float unicamente no acúmulo final via FMA. (Corte de ~40% de intruções de clock cycle na hot-path em comparação à casts de int16_t para float).
*   **Busca em Dois Estágios**: A busca mapeia apenas os `nprobe=5` clusters mais próximos de cara. Se o resultado do limite gerar incerteza (retornar entre 2 e 3 fraudes), faz *fallback* em tempo real para os `nprobe=24` clusters mais próximos.
*   **Early Termination**: Descarta blocos aos montes, parando os cálculos imediatamente se a distância parcial nas primeiras 8/14 dims exceder a 5ª pior distância guardada.

### 2. HTTP & Event Loop io_uring
*   **io_uring Multiplexing**: Elimina as centenas de sys-calls `read()` e `write()` atreladas ao velho `epoll` acoplando as requisições nativamente em um ring buffer compartilhado entre *Kernel/User Space*.
*   **Thread Dedicada e SCM_RIGHTS**: Uma p-thread roda paralelamente aceitando FDs enviadas por Load Balancers através de *Unix Domain Sockets*, e enfileira requests passando um bump ao ring buffer de `io_uring` notificando a *main thread*.
*   **Zero-Allocation JSON Parser**: Parser manual que processa strings JSON inteiramente por ponteiros (zero bytes alocados em RAM).
*   **Respostas Estáticas Otimizadas**: Strings contendo o Header HTTP já populadas com os status e content-lengths cacheadas sem cálculo adicional, entregues diretamente do bloco `.rodata`.

## Configuração

| Variável | Padrão | Descrição |
|----------|---------|-----------|
| `IVF_NPROBE` | `5` | Clusters sondados na passada rápida |
| `IVF_FULL_NPROBE` | `24` | Clusters sondados na passada completa (fallback) |
| `INDEX_PATH` | `resources/index.bin` | Caminho do índice bin |

## Estrutura

```
├── src/
│   ├── main.c              # Inicialização do loop e ponteiros UDS
│   ├── config.c / .h       # Configuração via env
│   ├── http_server.c / .h  # Servidor HTTP assíncrono io_uring
│   ├── http_resp.c / .h    # Respostas e status-codes pré-computados
│   ├── scm_rights.c / .h   # Controle socket (FD pass por TCP via UDS)
│   ├── vectorizer.c / .h   # JSON inline-parser custom
│   └── knn.c / .h          # Motor Inverted File L2 de vetores (AVX2/int16)
├── test/
│   └── test_*.c            # Suite de asserts do indexador
├── resources/
│   └── index.bin.gz        # Base clusterizada dos vetores (K=4096)
├── Dockerfile
├── docker-compose.yml
└── autoresearch.sh         # Script benchmark (k6 runtime)
```

## Build Local

```bash
make clean && make
```
O build invoca `-O3 -march=haswell -mtune=haswell -flto -static` — agressivamente otimizado p/ Haswell (AVX2, sem dependências em runtime para imagem de container).

## CI/CD e Ambientes de Teste

Mac Mini Late 2014 (2.6 GHz Haswell, 8 GB RAM, Ubuntu 24.04). Limites de Benchmark avaliados para `1.0 CPU` combinados entre APIs e LB. O Actions publica a compilação diretamente no Github Container Registry (GHCR).

## Licença

MIT
