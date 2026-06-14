# IICPC 2026 — Exchange Benchmark Arena

> A High-Performance, Distributed Benchmarking and Hosting Platform for evaluating contestant-submitted trading infrastructure.

---

## Architecture Overview

The platform uses a decoupled, event-driven C++20 microservices architecture to process submissions, generate trading load, audit correctness, and stream live standings updates:

```
┌─────────────┐     ┌──────────────┐     ┌───────────────┐
│   Frontend   │────▶│ API Gateway  │────▶│ Submission Svc│──▶ MinIO / GCS
│  (React+WS)  │     │ (Drogon C++) │     │ (Drogon C++ ) │
└──────┬───────┘     └──────┬───────┘     └───────────────┘
       │                    │
       │ WebSocket          │ REST
       ▼                    ▼
┌──────────────┐     ┌──────────────┐     ┌───────────────┐
│ Leaderboard  │◀────│  Sandbox Mgr │────▶│  Contestant   │
│  Service     │     │ (Drogon C++) │     │   Exchange    │
│ (Redis PubSub)     └──────────────┘     │ (gVisor Pod)  │
└──────────────┘                          └───────┬───────┘
       ▲                                          │
       │ Redis                                    │ HTTP
       │                                          ▼
┌──────────────┐     ┌──────────────┐     ┌───────────────┐
│   Scoring    │◀────│   Redpanda   │◀────│  Bot Worker   │
│    Engine    │     │  (Streaming) │     │ (1000+ bots)  │
│ (HDR Histo)  │     └──────────────┘     └───────────────┘
└──────────────┘                                  ▲
       │                                          │
       ▼                                   ┌──────┴───────┐
┌──────────────┐                           │     Bot      │
│ TimescaleDB  │                           │ Orchestrator │
│ (Telemetry)  │                           │   (Phases)   │
└──────────────┘                           └──────────────┘
```

---

## Tech Stack

| Component | Technology | Rationale |
| :--- | :--- | :--- |
| **Backend Core** | C++20 / Drogon Framework | Extremely low overhead, high concurrency, and sub-millisecond response latencies. |
| **Streaming** | Redpanda (Kafka-compatible)| Low-latency message brokerage for telemetry streams. |
| **Time-Series** | TimescaleDB | Multi-dimensional partitioning and time-series aggregation for logs. |
| **Leaderboard** | Redis Sorted Sets | `O(log N)` standings computation and sub-millisecond Pub/Sub propagation. |
| **Object Store** | MinIO (S3-compatible) / GCS | Secure binary archive storage. |
| **Sandboxing** | gVisor (GKE Sandbox) | Strong syscall interception and user-space kernel isolation. |
| **Orchestration**| Kubernetes | Scalable orchestration for runners, isolated namespaces, and network policies. |
| **IaC** | Terraform + Helm | Declarative VPC, Cloud NAT, GKE and Helm-driven chart deployments. |
| **Frontend** | React + Vite + Recharts | Responsive, WebSocket-driven live-graph scoreboard. |

---

## Local Quick Start

### Prerequisites
- GCC 11+ or Clang 13+ (supporting C++20)
- CMake 3.22+
- `vcpkg` package manager (configured and added to path)
- Docker & Docker Compose
- Node.js 18+

### 1. Start Local Infrastructure
Start PostgreSQL, TimescaleDB, Redis, Redpanda, and MinIO:
```bash
make up
```

### 2. Build the C++ Backend Services
Compile the microservices via CMake (auto-detects `vcpkg` toolchain):
```bash
# Optional: Set toolchain location if not auto-detected
# export VCPKG_TOOLCHAIN="/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake"

make build
```

### 3. Run the E2E Demo
Start a mock contestant exchange, launch all services, run a 30-second load test, and output telemetry:
```bash
make demo
```

### 4. Start the Frontend Dashboard
```bash
cd frontend
npm install
npm run dev
```
Open [http://localhost:3000](http://localhost:3000) to view the live dashboard.

---

## Development & Utility Commands

Use the provided `Makefile` to simplify C++ building and running:

*   **Build a specific service**:
    ```bash
    make build-gateway
    make build-scoring-engine
    ```
*   **Compile and run a service locally**:
    ```bash
    make dev-gateway
    make dev-submission-svc
    ```
*   **Run unit tests**:
    ```bash
    make test
    ```
*   **Clean build targets & docker volumes**:
    ```bash
    make clean
    ```

---

## Production Cloud Deployment (GCP / GKE)

The platform is designed to deploy to Google Cloud Platform with strict contestant sandboxing. Files are located in `deploy/gcp/`.

### Deployment Steps:
1. **Configure gcloud SDK**:
   Ensure you have authenticated with your GCP project:
   ```bash
   gcloud auth login
   gcloud config set project YOUR_PROJECT_ID
   ```
2. **Build and Push Microservices**:
   Build the Docker images and push them to Google Artifact Registry:
   ```bash
   ./deploy/gcp/build_and_push.sh
   ```
3. **Launch Deployment Script**:
   The automated script handles Terraform configuration, GKE provisioning, Secrets creation, Network Policy binding, and Helm application:
   ```bash
   cd deploy/gcp
   ./deploy_cloud.sh
   ```

*Detailed cloud documentation is available in the [Technical Design Submission Document](docs/hackathon_design_submission.md).*

---

## Scoring Formula

A contestant's composite score $S_c$ is calculated as:

$$S_c = 0.4 \cdot S_{\text{latency}} + 0.3 \cdot S_{\text{throughput}} + 0.3 \cdot S_{\text{correctness}}$$

Where:
*   **Latency ($S_{\text{latency}}$)**: Evaluated using HDR Histograms at the 99th percentile ($P_{99}$).
    $$S_{\text{latency}} = \min\left(1.0, \frac{100\text{ ms}}{P_{99} \text{ Latency}}\right)$$
*   **Throughput ($S_{\text{throughput}}$)**: Rolling peak Transaction Per Second (TPS) scaled against target TPS.
    $$S_{\text{throughput}} = \min\left(1.0, \frac{\text{Peak TPS}}{10,000}\right)$$
*   **Correctness ($S_{\text{correctness}}$)**: Ratio of filled orders that matched the reference order book validation checks.
    $$S_{\text{correctness}} = \frac{\text{Valid Orders}}{\text{Total Orders}}$$

---

## API Endpoints

| Method | Endpoint | Description |
|:---|:---|:---|
| `POST` | `/api/v1/submissions` | Upload exchange code (.tar.gz) |
| `GET` | `/api/v1/submissions/{id}` | Get submission status |
| `POST` | `/api/v1/submissions/{id}/benchmark`| Trigger benchmark run |
| `GET` | `/api/v1/leaderboard` | Get current leaderboard (JSON) |
| `WS` | `/api/v1/leaderboard/stream` | Live leaderboard websocket updates |
| `GET` | `/api/v1/telemetry/{run_id}` | Get run metrics |
| `GET` | `/api/v1/telemetry/{run_id}/histogram`| Get latency distribution |

---

## License

MIT
