# IICPC 2026 — Exchange Benchmark Arena

> A Distributed Benchmarking and Hosting Platform for evaluating contestant-submitted trading infrastructure.

## Architecture Overview

```
┌─────────────┐     ┌──────────────┐     ┌───────────────┐
│   Frontend   │────▶│   Gateway    │────▶│ Submission Svc│──▶ MinIO
│  (React+WS)  │     │   (chi)      │     │  (Upload/DB)  │
└──────┬───────┘     └──────┬───────┘     └───────────────┘
       │                    │
       │ WebSocket          │ REST
       ▼                    ▼
┌──────────────┐     ┌──────────────┐     ┌───────────────┐
│ Leaderboard  │◀────│  Sandbox Mgr │────▶│  Contestant   │
│  Service     │     │ (Kaniko/K8s) │     │   Exchange    │
│ (Redis PubSub)     └──────────────┘     │  (gVisor Pod) │
└──────────────┘                          └───────┬───────┘
       ▲                                          │
       │ Redis                                    │ HTTP
       │                                          ▼
┌──────────────┐     ┌──────────────┐     ┌───────────────┐
│   Scoring    │◀────│  Redpanda    │◀────│  Bot Worker   │
│   Engine     │     │  (Streaming) │     │ (1000+ bots)  │
│ (HDR Histo)  │     └──────────────┘     └───────────────┘
└──────────────┘                                  ▲
       │                                          │
       ▼                                   ┌──────┴───────┐
┌──────────────┐                           │     Bot      │
│ TimescaleDB  │                           │ Orchestrator │
│ (Telemetry)  │                           │  (Phases)    │
└──────────────┘                           └──────────────┘
```

## Tech Stack

| Component          | Technology                 | Rationale                              |
|--------------------|----------------------------|----------------------------------------|
| **Language**        | Go 1.22                    | Goroutine-based concurrency, low GC    |
| **Streaming**       | Redpanda                   | C++ single-binary, <1ms tail latency   |
| **Time-Series**     | TimescaleDB                | Continuous aggregates, compression     |
| **Leaderboard**     | Redis Sorted Sets          | O(log N) ZADD, Pub/Sub for WebSocket   |
| **Object Store**    | MinIO (S3-compat)          | Submission archive storage             |
| **Sandboxing**      | gVisor + Kaniko            | User-space kernel + rootless builds    |
| **Orchestration**   | Kubernetes                 | Per-submission namespaces, KEDA        |
| **IaC**             | Terraform + Helm           | GKE, Cloud SQL, Memorystore            |
| **Frontend**        | React + Vite + Recharts    | Real-time WebSocket leaderboard        |

## Quick Start

### Prerequisites
- Go 1.22+
- Docker & Docker Compose
- Node.js 18+

### 1. Start Infrastructure
```bash
make up
```
This starts PostgreSQL, TimescaleDB, Redis, Redpanda, and MinIO locally.

### 2. Run the Demo
```bash
make demo
```
This script:
1. Starts the Go exchange template (test target)
2. Launches all platform services
3. Triggers a 30-second benchmark with 50 bots
4. Shows live telemetry output

### 3. Start Frontend
```bash
cd frontend && npm install && npm run dev
```
Open http://localhost:3000 for the live leaderboard.

## Project Structure

```
trading-platform/
├── services/              # Go microservices
│   ├── gateway/           # API reverse proxy (port 8090)
│   ├── submission-svc/    # Code upload & storage (port 8091)
│   ├── bot-orchestrator/  # Benchmark coordination (port 8092)
│   ├── leaderboard-svc/   # WebSocket leaderboard (port 8093)
│   ├── scoring-engine/    # Telemetry aggregation (port 8094)
│   ├── sandbox-mgr/       # Container build & deploy (port 8095)
│   └── bot-worker/        # Trading bot fleet (no HTTP)
├── pkg/                   # Shared Go packages
│   ├── models/            # Domain types (Order, Telemetry, etc.)
│   ├── orderbook/         # Reference order book for validation
│   ├── telemetry/         # Topic names & event types
│   └── botprofiles/       # Predefined bot strategies
├── templates/
│   └── go-exchange/       # Sample contestant exchange
├── frontend/              # React real-time dashboard
├── deploy/
│   ├── docker-compose.yml # Local dev infrastructure
│   ├── dockerfiles/       # Service Dockerfiles
│   ├── migrations/        # SQL migrations
│   ├── terraform/         # GKE/GCP provisioning
│   └── helm/              # Kubernetes deployment
├── scripts/
│   └── run-demo.sh        # End-to-end demo script
├── go.work                # Go workspace
└── Makefile               # Build/test/deploy commands
```

## Scoring Formula

```
Composite = 0.4 × Latency + 0.3 × Throughput + 0.3 × Correctness
```

- **Latency** (40%): `min(1.0, 100ms / p99_latency_ms)` — Lower is better
- **Throughput** (30%): `min(1.0, max_tps / 10000)` — Higher is better
- **Correctness** (30%): `successful_orders / total_orders` — Price-time priority validation

## API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/api/v1/submissions` | Upload exchange code (.tar.gz) |
| `GET` | `/api/v1/submissions/{id}` | Get submission status |
| `POST` | `/api/v1/submissions/{id}/benchmark` | Trigger benchmark run |
| `GET` | `/api/v1/leaderboard` | Get current leaderboard (JSON) |
| `WS` | `/api/v1/leaderboard/stream` | Live leaderboard updates |
| `GET` | `/api/v1/telemetry/{run_id}` | Get run metrics |
| `GET` | `/api/v1/telemetry/{run_id}/histogram` | Get latency distribution |

## License

MIT
