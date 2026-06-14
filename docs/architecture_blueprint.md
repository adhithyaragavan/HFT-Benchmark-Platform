# IICPC Exchange Benchmark Arena — Architecture Blueprint

This document defines the production-grade, distributed architecture of the IICPC Exchange Benchmark Arena. The platform is designed to securely upload, containerize, host, stress-test, and evaluate contestant-submitted trading infrastructure at scale.

---

## 1. Architectural Overview

The system is designed as a decoupled, asynchronous microservices architecture that uses event streaming to ingest telemetry events and WebSocket-driven updates for real-time dashboarding.

```
                  ┌───────────────────────┐
                  │   Contestant Client   │
                  └───────────┬───────────┘
                              │ HTTP / WS
                              ▼
┌──────────────────────────────────────────────────────────────┐
│                    API Gateway (Drogon C++)                  │
└──────┬──────────────────────┬─────────────────────────┬──────┘
       │ REST                 │ REST                    │ REST
       ▼                      ▼                         ▼
┌──────────────┐       ┌──────────────┐          ┌──────────────┐
│  Submission  │       │  Leaderboard │          │     Bot      │
│  Svc (C++)   │       │  Svc (C++)   │          │ Orchestrator │
└──────┬───────┘       └──────▲───────┘          └──────┬───────┘
       │                      │ Redis                   │ Kafka
       ▼                      │ Sorted Set              ▼
┌──────────────┐       ┌──────┴───────┐          ┌──────────────┐
│ Sandbox Mgr  │       │    Redis     │◄──────── │   Scoring    │
│    (C++)     │       │ (PubSub/KV)  │          │ Engine (C++) │
└──────┬───────┘       └──────────────┘          └──────▲───────┘
       │                                                │ Kafka
       │ K8s API                                        │ (telemetry-raw)
       ▼                                                │
┌──────────────────────────────────────────────┐ ┌──────┴───────┐
│              GKE Cluster (Kubernetes)        │ │  Bot Worker  │
│                                              │ │    Fleet     │
│ ┌──────────────────┐    ┌──────────────────┐ │ │ (1000+ bots) │
│ │  Contestant Pod  │◄───│  Bot Worker Pod  │ │ └──────┬───────┘
│ │ (gVisor Sandbox) │REST│    (KEDA-scaled) │ │        │ Kafka
│ └──────────────────┘    └──────────────────┘ │        ▼
└──────────────────────────────────────────────┘ └────────────────┘
```

---

## 2. Microservice Specifications

All microservices are implemented in high-performance C++20 utilizing the Drogon web framework for extreme request handling capabilities, low memory footprint, and low tail-latencies.

| Service Name | Primary Language / Framework | External Port | Role |
| :--- | :--- | :--- | :--- |
| **API Gateway** | C++ / Drogon | `8090` | Acts as the reverse proxy for all client traffic, handling CORS, pre-flight requests, and request-id tracing. |
| **Submission Service**| C++ / Drogon | `8091` | Manages registration, code zip uploads, database metadata (PostgreSQL), and archive storage (MinIO). |
| **Sandbox Manager** | C++ / Drogon | `8095` | Orchestrates containerization. Builds Docker images and deploys them to local docker engine or GKE/Kubernetes. |
| **Bot Orchestrator** | C++ / Drogon | `8092` | Coordinates the lifecycle of a benchmark run (Warmup, Ramp, Peak, Cooldown) and schedules bot instructions via Kafka. |
| **Bot Worker** | C++ / Native | N/A (Kafka Consumer) | Scalable worker daemon that consumes command events and spawns concurrent mock trading bots to load-test contestants. |
| **Scoring Engine** | C++ / Native | `8094` | Consumes raw telemetry, validates matching engine correctness, calculates latency distribution, and updates Redis/TimescaleDB. |
| **Leaderboard Service**| C++ / Drogon | `8093` | Manages WebSockets and serves real-time standings computed from Redis Sorted Sets. |

---

## 3. Data Storage Strategy

The database architectures are separated by workload type to guarantee performance during high-throughput stress tests:

1. **Metadata Store (PostgreSQL)**:
   * **Purpose**: Manages system state, team profiles, submission records, and test run metadata.
   * **Access Pattern**: Low frequency, high durability, relational queries.
2. **Telemetry Store (TimescaleDB)**:
   * **Purpose**: Ingests raw telemetry events (order acknowledgments, cancels, fills) for audit trails and detailed historical reports.
   * **Feature**: Employs hyper-tables partitioned by timestamp for rapid bulk inserts and efficient down-sampling/compression.
3. **Leaderboard Cache (Redis)**:
   * **Purpose**: Real-time standings caching and pub/sub.
   * **Feature**: Leverages Sorted Sets (`ZADD`, `ZRANGE`) for `O(log N)` leaderboard ranking, and Redis Pub/Sub to broadcast updates to the WebSocket server fleet.
4. **Binary & Source Store (MinIO / S3)**:
   * **Purpose**: Highly durable object storage for contestant submitted archives and sandbox build logs.

---

## 4. Telemetry Ingestion & Score Calculation

To handle thousands of concurrent transactions without impacting latency tracking, the telemetry system utilizes an asynchronous pipeline built on Redpanda/Kafka:

```
[Bot Worker] ──► (Kafka: telemetry-raw) ──► [Scoring Engine] ──► [Redis ZSET] ──► [Leaderboard WS]
```

### Composite Score Formula
A contestant's composite score $S_c$ is calculated as a weighted combination of speed, stability, and correctness:
$$S_c = 0.4 \cdot S_{\text{latency}} + 0.3 \cdot S_{\text{throughput}} + 0.3 \cdot S_{\text{correctness}}$$

Where:
* **Latency Score ($S_{\text{latency}}$)**:
  $$S_{\text{latency}} = \min\left(1.0, \frac{100\text{ ms}}{P_{99}\text{ latency}}\right)$$
  Tracks order acknowledgment times using an optimized `hdr_histogram` implementation to avoid precision loss.
* **Throughput Score ($S_{\text{throughput}}$)**:
  $$S_{\text{throughput}} = \min\left(1.0, \frac{\text{Peak TPS}}{10,000}\right)$$
  Calculated over a rolling 1-second window.
* **Correctness Score ($S_{\text{correctness}}$)**:
  $$S_{\text{correctness}} = \frac{\text{Successful Orders}}{\text{Total Processed Orders}}$$
  Evaluates price-time priority violation and fill accuracy by mirroring trades against a reference order book in the platform.

---

## 5. Sandboxing & Isolation Strategy

To protect the host system and ensure fair resource allocation, contestant uploads are isolated at build time and during execution:

### 1. Build Sandboxing (Docker/Kaniko)
* Code compilation runs inside isolated builder containers.
* The Sandbox Manager limits resources during docker build: `--memory=2g` and `--cpu-quota=200000` (2 CPU cores) to prevent resource hogging.

### 2. Runtime Isolation (gVisor & Kernel Sandboxing)
* In Kubernetes mode, contestants are deployed inside pods configured with **gVisor** (`RuntimeClass: gvisor`). This intercepts system calls, protecting the host Linux kernel.
* Locally, containers are run with restricted permissions: `--read-only`, drops all capabilities (`--cap-drop=ALL`), and blocks privilege escalation (`--security-opt=no-new-privileges:true`).

### 3. Network Isolation
* All pods run inside dedicated namespaces `contestant-[id]`.
* A strict **NetworkPolicy** is enforced to deny all egress traffic:
  ```yaml
  apiVersion: networking.k8s.io/v1
  kind: NetworkPolicy
  metadata:
    name: deny-contestant-egress
  spec:
    podSelector: {}
    policyTypes:
    - Egress
  ```
  This ensures contestant submissions cannot dial home, scrape local network endpoints, or perform malicious communication.

### 4. Resource Allocation
* Kubernetes **ResourceQuotas** and CPU pinning limit contestant pods to a maximum of 2 CPUs and 2GiB RAM.
