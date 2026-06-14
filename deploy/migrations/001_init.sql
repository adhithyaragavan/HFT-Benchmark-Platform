-- ============================================================================
-- IICPC 2026 — Distributed Benchmarking Platform
-- Migration 001: Core metadata tables (PostgreSQL 16)
-- ============================================================================

-- Enable UUID generation
CREATE EXTENSION IF NOT EXISTS "pgcrypto";

-- ─── Teams ──────────────────────────────────────────────────────────────────
CREATE TABLE teams (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name       VARCHAR(100) NOT NULL UNIQUE,
    api_key    VARCHAR(64)  NOT NULL UNIQUE,
    members    JSONB        NOT NULL DEFAULT '[]',
    created_at TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_teams_api_key ON teams (api_key);

-- ─── Submissions ────────────────────────────────────────────────────────────
CREATE TABLE submissions (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    team_id      UUID         NOT NULL REFERENCES teams(id) ON DELETE CASCADE,
    language     VARCHAR(10)  NOT NULL CHECK (language IN ('cpp', 'rust', 'go')),
    status       VARCHAR(20)  NOT NULL DEFAULT 'pending'
                     CHECK (status IN ('pending', 'building', 'ready', 'failed', 'running', 'completed')),
    source_url   TEXT         NOT NULL,
    image_tag    TEXT,
    endpoint_url TEXT,
    namespace    TEXT,
    error_msg    TEXT,
    created_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_submissions_team    ON submissions (team_id);
CREATE INDEX idx_submissions_status  ON submissions (status);

-- ─── Benchmark Runs ─────────────────────────────────────────────────────────
CREATE TABLE benchmark_runs (
    id                UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    submission_id     UUID         NOT NULL REFERENCES submissions(id) ON DELETE CASCADE,
    status            VARCHAR(20)  NOT NULL DEFAULT 'pending'
                          CHECK (status IN ('pending', 'warmup', 'ramping', 'sustained', 'cooldown', 'scoring', 'completed', 'failed')),
    bot_count         INTEGER      NOT NULL,
    duration_seconds  INTEGER      NOT NULL,
    started_at        TIMESTAMPTZ,
    completed_at      TIMESTAMPTZ,
    -- Aggregated results
    composite_score   DOUBLE PRECISION,
    latency_p50_us    BIGINT,
    latency_p90_us    BIGINT,
    latency_p99_us    BIGINT,
    max_tps           BIGINT,
    avg_tps           BIGINT,
    correctness_score DOUBLE PRECISION,
    total_orders      BIGINT,
    successful_orders BIGINT,
    failed_orders     BIGINT,
    -- Configuration snapshot
    bot_profile       JSONB,
    error_msg         TEXT,
    created_at        TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_benchmark_runs_submission ON benchmark_runs (submission_id);
CREATE INDEX idx_benchmark_runs_status     ON benchmark_runs (status);
CREATE INDEX idx_benchmark_runs_score      ON benchmark_runs (composite_score DESC NULLS LAST);

-- ─── Leaderboard (materialized view of best scores) ─────────────────────────
CREATE TABLE leaderboard (
    team_id          UUID PRIMARY KEY REFERENCES teams(id) ON DELETE CASCADE,
    team_name        VARCHAR(100) NOT NULL,
    best_run_id      UUID REFERENCES benchmark_runs(id),
    composite_score  DOUBLE PRECISION NOT NULL DEFAULT 0,
    latency_p99_us   BIGINT,
    max_tps          BIGINT,
    correctness      DOUBLE PRECISION,
    rank             INTEGER,
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_leaderboard_score ON leaderboard (composite_score DESC);

-- ─── Helper function: update updated_at timestamp ───────────────────────────
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER update_teams_updated_at
    BEFORE UPDATE ON teams
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_submissions_updated_at
    BEFORE UPDATE ON submissions
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

-- ─── Seed a demo team for testing ───────────────────────────────────────────
INSERT INTO teams (name, api_key, members)
VALUES ('Demo Team', 'demo-api-key-1234567890abcdef', '["Alice", "Bob"]')
ON CONFLICT (name) DO NOTHING;
