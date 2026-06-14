-- ============================================================================
-- IICPC 2026 — Distributed Benchmarking Platform
-- Migration 002: Telemetry time-series tables (TimescaleDB)
-- ============================================================================

-- Enable TimescaleDB extension
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- ─── Raw Telemetry Hypertable ───────────────────────────────────────────────
-- Each row represents a single order->response measurement from a bot
CREATE TABLE telemetry_raw (
    time        TIMESTAMPTZ NOT NULL,
    run_id      UUID        NOT NULL,
    bot_id      TEXT        NOT NULL,
    order_id    TEXT        NOT NULL,
    order_type  TEXT        NOT NULL,  -- 'limit_buy', 'limit_sell', 'market_buy', 'market_sell', 'cancel'
    latency_ns  BIGINT      NOT NULL,  -- nanoseconds from send to ack
    success     BOOLEAN     NOT NULL,
    error_code  TEXT,
    symbol      TEXT        NOT NULL DEFAULT 'AAPL'
);

-- Convert to hypertable with 1-minute chunks
SELECT create_hypertable('telemetry_raw', 'time',
    chunk_time_interval => INTERVAL '1 minute'
);

-- Index for querying by run
CREATE INDEX idx_telemetry_run_time ON telemetry_raw (run_id, time DESC);

-- ─── Continuous Aggregate: 1-second buckets ─────────────────────────────────
-- Pre-computed aggregations for the real-time dashboard
CREATE MATERIALIZED VIEW telemetry_1s
WITH (timescaledb.continuous) AS
SELECT
    time_bucket('1 second', time) AS bucket,
    run_id,
    COUNT(*)                                   AS total_orders,
    COUNT(*) FILTER (WHERE success = true)     AS successful_orders,
    COUNT(*) FILTER (WHERE success = false)    AS failed_orders,
    AVG(latency_ns)::BIGINT                    AS avg_latency_ns,
    MIN(latency_ns)                            AS min_latency_ns,
    MAX(latency_ns)                            AS max_latency_ns,
    -- Approximate percentiles using TimescaleDB toolkit (if available)
    -- Otherwise, we compute exact percentiles in the scoring engine
    percentile_cont(0.50) WITHIN GROUP (ORDER BY latency_ns)::BIGINT AS p50_latency_ns,
    percentile_cont(0.90) WITHIN GROUP (ORDER BY latency_ns)::BIGINT AS p90_latency_ns,
    percentile_cont(0.99) WITHIN GROUP (ORDER BY latency_ns)::BIGINT AS p99_latency_ns
FROM telemetry_raw
GROUP BY bucket, run_id
WITH NO DATA;

-- Auto-refresh: compute every 2 seconds, covering data up to 2 seconds ago
SELECT add_continuous_aggregate_policy('telemetry_1s',
    start_offset    => INTERVAL '30 seconds',
    end_offset      => INTERVAL '2 seconds',
    schedule_interval => INTERVAL '2 seconds'
);

-- ─── Continuous Aggregate: 10-second buckets (for historical view) ──────────
CREATE MATERIALIZED VIEW telemetry_10s
WITH (timescaledb.continuous) AS
SELECT
    time_bucket('10 seconds', time) AS bucket,
    run_id,
    COUNT(*)                                   AS total_orders,
    COUNT(*) FILTER (WHERE success = true)     AS successful_orders,
    AVG(latency_ns)::BIGINT                    AS avg_latency_ns,
    MAX(latency_ns)                            AS max_latency_ns,
    percentile_cont(0.50) WITHIN GROUP (ORDER BY latency_ns)::BIGINT AS p50_latency_ns,
    percentile_cont(0.90) WITHIN GROUP (ORDER BY latency_ns)::BIGINT AS p90_latency_ns,
    percentile_cont(0.99) WITHIN GROUP (ORDER BY latency_ns)::BIGINT AS p99_latency_ns
FROM telemetry_raw
GROUP BY bucket, run_id
WITH NO DATA;

SELECT add_continuous_aggregate_policy('telemetry_10s',
    start_offset    => INTERVAL '5 minutes',
    end_offset      => INTERVAL '10 seconds',
    schedule_interval => INTERVAL '10 seconds'
);

-- ─── Correctness Validation Results ─────────────────────────────────────────
CREATE TABLE correctness_events (
    time          TIMESTAMPTZ NOT NULL,
    run_id        UUID        NOT NULL,
    order_id      TEXT        NOT NULL,
    check_type    TEXT        NOT NULL,  -- 'price_priority', 'time_priority', 'fill_accuracy', 'invariant'
    passed        BOOLEAN     NOT NULL,
    expected      TEXT,
    actual        TEXT,
    details       TEXT
);

SELECT create_hypertable('correctness_events', 'time',
    chunk_time_interval => INTERVAL '1 minute'
);

CREATE INDEX idx_correctness_run ON correctness_events (run_id, time DESC);

-- ─── Retention Policies ─────────────────────────────────────────────────────
-- Drop raw telemetry after 24 hours (aggregates are retained longer)
SELECT add_retention_policy('telemetry_raw', INTERVAL '24 hours');

-- Drop correctness events after 7 days
SELECT add_retention_policy('correctness_events', INTERVAL '7 days');

-- ─── Compression (enable on chunks older than 1 hour) ───────────────────────
ALTER TABLE telemetry_raw SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'run_id',
    timescaledb.compress_orderby = 'time DESC'
);

SELECT add_compression_policy('telemetry_raw', INTERVAL '1 hour');
