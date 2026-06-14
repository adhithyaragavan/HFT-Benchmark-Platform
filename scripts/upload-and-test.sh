#!/usr/bin/env bash
# ============================================================================
# IICPC 2026 — Automated Submission Upload & Stress Test Runner
# ============================================================================
set -euo pipefail

GATEWAY_URL="http://localhost:8090"

if [ "$#" -lt 4 ]; then
  echo "Usage: $0 <language> <team_name> <email> <archive_file_path>"
  echo ""
  echo "Examples:"
  echo "  $0 go test-go-team go-test@example.com /Users/adhithyaragavan/.gemini/antigravity/scratch/valid_go_sample_fixed.tar.gz"
  echo "  $0 rust test-rust-team rust-test@example.com /Users/adhithyaragavan/.gemini/antigravity/scratch/sample_rust_exchange.tar.gz"
  echo "  $0 go test-slow-team slow-test@example.com /Users/adhithyaragavan/.gemini/antigravity/scratch/sample_slow_exchange.tar.gz"
  exit 1
fi

LANGUAGE="$1"
TEAM_NAME="$2"
EMAIL="$3"
FILE_PATH="$4"

if [ ! -f "$FILE_PATH" ]; then
  echo "Error: File $FILE_PATH does not exist"
  exit 1
fi

echo "=== Step 1: Uploading submission archive ($LANGUAGE) ==="
# POST the file upload to the API Gateway
RESPONSE=$(curl -sf -F "language=$LANGUAGE" \
  -F "team_id=$TEAM_NAME" \
  -F "email=$EMAIL" \
  -F "file=@$FILE_PATH" \
  "$GATEWAY_URL/api/submit")

echo "Upload successful!"
echo "Server Response: $RESPONSE"

SUBMISSION_ID=$(echo "$RESPONSE" | grep -oE '"id":"[0-9a-fA-F-]*"' | head -1 | cut -d'"' -f4)
echo "Generated Submission ID: $SUBMISSION_ID"
echo ""

echo "=== Step 2: Waiting for sandbox build and deployment ==="
while true; do
  STATUS_RESP=$(curl -sf "$GATEWAY_URL/api/v1/submissions/$SUBMISSION_ID")
  STATUS=$(echo "$STATUS_RESP" | grep -oE '"status":"[a-zA-Z0-9_-]*"' | head -1 | cut -d'"' -f4)
  echo "  [Build Status]: $STATUS"
  
  if [ "$STATUS" = "ready" ]; then
    ENDPOINT_URL=$(echo "$STATUS_RESP" | grep -oE '"endpoint_url":"[^"]*"' | head -1 | cut -d'"' -f4)
    echo "Sandbox container deployed successfully!"
    echo "Sandbox Endpoint: $ENDPOINT_URL"
    break
  elif [ "$STATUS" = "failed" ]; then
    ERROR_MSG=$(echo "$STATUS_RESP" | grep -oE '"error_msg":"[^"]*"' | head -1 | cut -d'"' -f4)
    echo "Error: Sandbox container build failed: $ERROR_MSG"
    exit 1
  fi
  sleep 3
done
echo ""

RUN_ID="run-$(date +%s)"
echo "=== Step 3: Triggering Stress-Test Benchmark ($RUN_ID) ==="
BENCH_RESP=$(curl -sf -X POST "$GATEWAY_URL/api/v1/submissions/$SUBMISSION_ID/benchmark" \
  -H "Content-Type: application/json" \
  -d "{
    \"run_id\": \"$RUN_ID\",
    \"duration_seconds\": 30,
    \"bot_count\": 50,
    \"profile\": {
      \"limit_ratio\": 0.6,
      \"market_ratio\": 0.3,
      \"cancel_ratio\": 0.1,
      \"orders_per_sec_per_bot\": 10,
      \"symbols\": [\"AAPL\", \"GOOG\", \"MSFT\"]
    }
  }")

echo "Benchmark triggered successfully!"
echo "Status response: $BENCH_RESP"
echo ""

echo "=== Step 4: Monitoring stress test execution ==="
while true; do
  ORCH_RESP=$(curl -sf "$GATEWAY_URL/api/v1/orchestrator/status/$RUN_ID")
  RUN_STATUS=$(echo "$ORCH_RESP" | grep -oE '"status":"[a-zA-Z0-9_-]*"' | head -1 | cut -d'"' -f4)
  PHASE=$(echo "$ORCH_RESP" | grep -oE '"current_phase":"[a-zA-Z0-9_-]*"' | head -1 | cut -d'"' -f4)
  echo "  [Test Status]: $RUN_STATUS | Current Phase: $PHASE"
  
  if [ "$RUN_STATUS" = "completed" ]; then
    echo "Stress test run complete!"
    break
  elif [ "$RUN_STATUS" = "failed" ]; then
    echo "Error: Stress test aborted or failed."
    exit 1
  fi
  sleep 4
done
echo ""

echo "=== Step 5: Finalizing score calculation ==="
FINALIZE_RESP=$(curl -sf -X POST "$GATEWAY_URL/api/v1/benchmark/$RUN_ID/telemetry/finalize" \
  -H "Content-Type: application/json" \
  -d "{\"team_id\": \"$TEAM_NAME\"}")

echo "Final Score & Telemetry standings registered:"
echo "$FINALIZE_RESP"
echo ""
echo "Standings have been published to the leaderboard. Check http://localhost:3000 to see it live!"
