#!/usr/bin/env bash
# ============================================================================
# IICPC 2026 — End-to-End Demo Script (C++ Version)
# ============================================================================
# This script demonstrates the complete pipeline:
# 1. Start infrastructure (Docker Compose)
# 2. Build the C++ services and the C++ exchange template using CMake/vcpkg
# 3. Start the C++ exchange template as a test target
# 4. Start all C++ microservices
# 5. Trigger a benchmark run via curl to API Gateway
# 6. Watch live telemetry & leaderboard updates
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

GATEWAY_URL="http://localhost:8090"
EXCHANGE_URL="http://localhost:8080"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log() { echo -e "${CYAN}[DEMO]${NC} $1"; }
success() { echo -e "${GREEN}[✓]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
error() { echo -e "${RED}[✗]${NC} $1"; }

# ─── Step 1: Check prerequisites ────────────────────────────────────
echo ""
echo -e "${BOLD}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║     IICPC 2026 — Exchange Benchmark Arena Demo (C++)     ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

log "Checking prerequisites..."
command -v docker >/dev/null 2>&1 || { error "Docker not found"; exit 1; }
command -v cmake >/dev/null 2>&1 || { error "CMake not found"; exit 1; }
command -v make >/dev/null 2>&1 || { error "make not found"; exit 1; }

# Auto-detect vcpkg toolchain
VCPKG_TOOLCHAIN=""
if [ -n "${VCPKG_TOOLCHAIN_PATH:-}" ]; then
  VCPKG_TOOLCHAIN="$VCPKG_TOOLCHAIN_PATH"
elif [ -f "$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then
  VCPKG_TOOLCHAIN="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
elif [ -f "/usr/local/share/vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then
  VCPKG_TOOLCHAIN="/usr/local/share/vcpkg/scripts/buildsystems/vcpkg.cmake"
elif [ -f "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then
  VCPKG_TOOLCHAIN="/opt/vcpkg/scripts/buildsystems/vcpkg.cmake"
fi

if [ -z "$VCPKG_TOOLCHAIN" ]; then
  error "vcpkg.cmake not found. Please set VCPKG_TOOLCHAIN_PATH or install vcpkg in ~/vcpkg."
  exit 1
fi

success "Prerequisites OK (vcpkg toolchain found: $VCPKG_TOOLCHAIN)"

# ─── Step 2: Start infrastructure ────────────────────────────────────
log "Starting infrastructure (PostgreSQL, TimescaleDB, Redis, Redpanda, MinIO)..."
cd "$PROJECT_DIR/deploy"
docker compose up -d
sleep 8
success "Infrastructure running"

# ─── Step 3: Create Redpanda topics ─────────────────────────────────
log "Creating Redpanda topics..."
docker exec platform-redpanda rpk topic create bot-commands --partitions 4 --replicas 1 2>/dev/null || true
docker exec platform-redpanda rpk topic create telemetry-raw --partitions 8 --replicas 1 2>/dev/null || true
docker exec platform-redpanda rpk topic create benchmark-status --partitions 2 --replicas 1 2>/dev/null || true
success "Redpanda topics created"

# ─── Step 4: Compile C++ targets ────────────────────────────────────
log "Compiling C++ services and exchange template (this might take a while on first run)..."
cmake -B "$PROJECT_DIR/build" -S "$PROJECT_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_MANIFEST_DIR="$PROJECT_DIR/services"
cmake --build "$PROJECT_DIR/build" --parallel "$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2)"
success "C++ binaries built successfully"

# ─── Step 5: Start the C++ exchange (test target) ──────────────────
log "Starting C++ exchange template..."
"$PROJECT_DIR/build/templates/cpp-exchange/cpp-exchange" &
EXCHANGE_PID=$!
sleep 2

# Health check
if curl -sf "$EXCHANGE_URL/api/v1/health" > /dev/null 2>&1; then
  success "Exchange running at $EXCHANGE_URL"
else
  error "Exchange failed to start"
  kill $EXCHANGE_PID 2>/dev/null
  exit 1
fi

# ─── Step 6: Start platform services ────────────────────────────────
log "Starting C++ platform services..."

"$PROJECT_DIR/build/services/gateway/gateway" &
GATEWAY_PID=$!
sleep 1

"$PROJECT_DIR/build/services/submission-svc/submission_svc" &
SUBMISSION_PID=$!
sleep 1

"$PROJECT_DIR/build/services/scoring-engine/scoring-engine" &
SCORING_PID=$!
sleep 1

"$PROJECT_DIR/build/services/leaderboard-svc/leaderboard-svc" &
LEADERBOARD_PID=$!
sleep 1

"$PROJECT_DIR/build/services/bot-orchestrator/bot-orchestrator" &
ORCHESTRATOR_PID=$!
sleep 1

"$PROJECT_DIR/build/services/bot-worker/bot_worker" &
WORKER_PID=$!
sleep 2

log "Starting React Frontend..."
cd "$PROJECT_DIR/frontend" && npm install >/dev/null 2>&1 && npm run dev -- --host --port 3000 &
FRONTEND_PID=$!
sleep 2

success "All services started"

# ─── Step 7: Run a benchmark ────────────────────────────────────────
log "Triggering benchmark run..."
echo ""

RESPONSE=$(curl -sf -X POST "$GATEWAY_URL/api/v1/orchestrator/start" \
  -H "Content-Type: application/json" \
  -d "{
    \"run_id\": \"123e4567-e89b-12d3-a456-426614174000\",
    \"submission_id\": \"demo-submission\",
    \"endpoint_url\": \"$EXCHANGE_URL\",
    \"bot_count\": 50,
    \"duration_seconds\": 30,
    \"profile\": {
      \"limit_ratio\": 0.6,
      \"market_ratio\": 0.3,
      \"cancel_ratio\": 0.1,
      \"orders_per_sec_per_bot\": 10,
      \"symbols\": [\"AAPL\", \"GOOG\", \"MSFT\"]
    }
  }" 2>/dev/null || echo '{"error":"could not reach orchestrator"}')

echo -e "  ${CYAN}Response:${NC} $RESPONSE"
echo ""

# ─── Step 8: Monitor telemetry ──────────────────────────────────────
log "Monitoring telemetry"
  # Poll telemetry for 60 seconds
  for i in $(seq 1 6); do
    sleep 10
    METRICS=$(curl -sf "$GATEWAY_URL/api/v1/benchmark/123e4567-e89b-12d3-a456-426614174000/telemetry" 2>/dev/null || echo '{}')
    echo -e "  [${i}0s] $METRICS"
  done

echo ""
success "Demo complete!"
echo ""
echo -e "  ${BOLD}Frontend:${NC}     http://localhost:3000"
echo -e "  ${BOLD}Gateway:${NC}      http://localhost:8090"
echo -e "  ${BOLD}Redpanda UI:${NC}  http://localhost:8088"
echo -e "  ${BOLD}MinIO Console:${NC} http://localhost:9001"
echo ""

# Cleanup on Ctrl+C or script exit
cleanup() {
  log "Shutting down..."
  kill $EXCHANGE_PID $GATEWAY_PID $SUBMISSION_PID $SCORING_PID $LEADERBOARD_PID $ORCHESTRATOR_PID $WORKER_PID $FRONTEND_PID 2>/dev/null || true
  cd "$PROJECT_DIR/deploy" && docker compose down
  success "Cleanup complete"
}

trap cleanup EXIT
wait
