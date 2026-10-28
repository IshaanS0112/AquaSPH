-- AquaSPH platform schema. docs/platform/TRD.md §5 explains each table;
-- the comments here explain constraints whose reason is not obvious.

CREATE TABLE tenants (
    id                  uuid PRIMARY KEY,
    name                text NOT NULL UNIQUE CHECK (name ~ '^[a-z0-9][a-z0-9_-]{0,63}$'),
    max_concurrent_jobs integer NOT NULL DEFAULT 2      CHECK (max_concurrent_jobs > 0),
    max_queued_jobs     integer NOT NULL DEFAULT 500    CHECK (max_queued_jobs > 0),
    max_particles       integer NOT NULL DEFAULT 400000 CHECK (max_particles > 0),
    max_wall_seconds    integer NOT NULL DEFAULT 900    CHECK (max_wall_seconds > 0),
    rate_per_minute     integer NOT NULL DEFAULT 600    CHECK (rate_per_minute > 0),
    rate_burst          integer NOT NULL DEFAULT 60     CHECK (rate_burst > 0),
    created_at          timestamptz NOT NULL DEFAULT now()
);

-- Only the SHA-256 of a key is stored (ADR-0004). The prefix is the lookup
-- handle and is safe to show in logs and UIs.
CREATE TABLE api_keys (
    id           uuid PRIMARY KEY,
    tenant_id    uuid NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    prefix       text NOT NULL UNIQUE CHECK (prefix ~ '^[a-z0-9]{8}$'),
    secret_hash  bytea NOT NULL CHECK (length(secret_hash) = 32),
    name         text NOT NULL DEFAULT '',
    created_at   timestamptz NOT NULL DEFAULT now(),
    last_used_at timestamptz,
    revoked_at   timestamptz
);
CREATE INDEX api_keys_tenant_idx ON api_keys (tenant_id);

CREATE TABLE sweeps (
    id             uuid PRIMARY KEY,
    tenant_id      uuid NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    scenario       text NOT NULL,
    quality        text NOT NULL,
    grid           jsonb NOT NULL,
    base_overrides jsonb NOT NULL DEFAULT '{}',
    total          integer NOT NULL CHECK (total > 0),
    labels         jsonb NOT NULL DEFAULT '{}',
    created_at     timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX sweeps_tenant_idx ON sweeps (tenant_id, id DESC);

CREATE TYPE job_state AS ENUM ('queued', 'running', 'completed', 'failed', 'cancelled');

CREATE TABLE jobs (
    id                  uuid PRIMARY KEY,
    tenant_id           uuid NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    sweep_id            uuid REFERENCES sweeps(id) ON DELETE CASCADE,
    sweep_params        jsonb,
    state               job_state NOT NULL DEFAULT 'queued',
    outcome             text CHECK (outcome IN ('stable', 'unstable')),
    priority            smallint NOT NULL DEFAULT 5 CHECK (priority BETWEEN 0 AND 9),

    -- What to run. spec is the fully resolved scenario, frozen at submit time: editing a
    -- catalogue file later cannot change what an existing job means.
    scenario            text NOT NULL,
    quality             text NOT NULL CHECK (quality IN ('low', 'medium', 'high')),
    sim_time            double precision CHECK (sim_time > 0),
    max_steps           integer CHECK (max_steps > 0),
    max_particles       integer NOT NULL CHECK (max_particles > 0),
    timeout_seconds     integer NOT NULL CHECK (timeout_seconds > 0),
    spec                jsonb NOT NULL,
    overrides           jsonb NOT NULL DEFAULT '{}',
    labels              jsonb NOT NULL DEFAULT '{}',
    spec_hash           text NOT NULL,
    allow_cache         boolean NOT NULL DEFAULT true,

    -- Execution. lease_token is the fencing token: every write a worker
    -- makes after claiming must match it (TRD §3).
    attempt             integer NOT NULL DEFAULT 0 CHECK (attempt >= 0),
    max_attempts        integer NOT NULL DEFAULT 3 CHECK (max_attempts > 0),
    run_after           timestamptz NOT NULL DEFAULT now(),
    lease_token         uuid,
    lease_expires_at    timestamptz,
    worker_id           uuid,
    cancel_requested_at timestamptz,
    progress            jsonb,

    -- Result. artifact_job_id names the job whose artifact directory holds
    -- the files: itself, or the source of a cache hit (ADR-0002).
    solver_id           text,
    cache_key           text,
    cache_hit           boolean NOT NULL DEFAULT false,
    source_job_id       uuid REFERENCES jobs(id) ON DELETE SET NULL,
    artifact_job_id     uuid,
    artifacts           jsonb NOT NULL DEFAULT '[]',
    result              jsonb,
    error_code          text,
    error_message       text,

    created_at          timestamptz NOT NULL DEFAULT now(),
    started_at          timestamptz,
    finished_at         timestamptz,
    updated_at          timestamptz NOT NULL DEFAULT now(),

    -- State invariants enforced by the database, so no code path, current or future, can write
    -- a running job without a lease or a completed job without an outcome.
    CONSTRAINT running_has_lease
        CHECK (state <> 'running' OR (lease_token IS NOT NULL AND lease_expires_at IS NOT NULL)),
    CONSTRAINT only_running_has_lease
        CHECK (state = 'running' OR lease_token IS NULL),
    CONSTRAINT terminal_has_finished_at
        CHECK (state NOT IN ('completed', 'failed', 'cancelled') OR finished_at IS NOT NULL),
    CONSTRAINT completed_has_outcome
        CHECK ((state = 'completed') = (outcome IS NOT NULL)),
    CONSTRAINT failed_has_error
        CHECK (state <> 'failed' OR error_code IS NOT NULL),
    CONSTRAINT cache_hit_has_source
        CHECK (NOT cache_hit OR (state = 'completed' AND artifact_job_id IS NOT NULL))
);

-- The claim query's index. Partial, so it holds only queued rows and stays
-- small however many finished jobs accumulate.
CREATE INDEX jobs_claim_idx          ON jobs (priority DESC, id) WHERE state = 'queued';
CREATE INDEX jobs_running_tenant_idx ON jobs (tenant_id) WHERE state = 'running';
CREATE INDEX jobs_queued_tenant_idx  ON jobs (tenant_id) WHERE state = 'queued';
CREATE INDEX jobs_lease_idx          ON jobs (lease_expires_at) WHERE state = 'running';
CREATE INDEX jobs_tenant_list_idx    ON jobs (tenant_id, id DESC);
CREATE INDEX jobs_sweep_idx          ON jobs (sweep_id, id) WHERE sweep_id IS NOT NULL;
CREATE INDEX jobs_cache_idx          ON jobs (cache_key, finished_at DESC) WHERE state = 'completed';
CREATE INDEX jobs_labels_idx         ON jobs USING gin (labels jsonb_path_ops);

CREATE FUNCTION jobs_touch_updated_at() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    NEW.updated_at := now();
    RETURN NEW;
END $$;

CREATE TRIGGER jobs_updated_at BEFORE UPDATE ON jobs
    FOR EACH ROW EXECUTE FUNCTION jobs_touch_updated_at();

-- Audit trail of every state transition, written by a trigger rather than
-- by application code so that no code path can forget to record one.
CREATE TABLE job_events (
    id         bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    job_id     uuid NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
    at         timestamptz NOT NULL DEFAULT clock_timestamp(),
    from_state job_state,
    to_state   job_state NOT NULL,
    attempt    integer NOT NULL,
    worker_id  uuid,
    detail     text
);
CREATE INDEX job_events_job_idx ON job_events (job_id, id);

CREATE FUNCTION jobs_record_transition() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'INSERT' THEN
        INSERT INTO job_events (job_id, from_state, to_state, attempt, worker_id, detail)
        VALUES (NEW.id, NULL, NEW.state, NEW.attempt, NEW.worker_id,
                CASE WHEN NEW.cache_hit THEN 'cache hit' END);
    ELSIF NEW.state IS DISTINCT FROM OLD.state THEN
        INSERT INTO job_events (job_id, from_state, to_state, attempt, worker_id, detail)
        VALUES (NEW.id, OLD.state, NEW.state, NEW.attempt,
                COALESCE(NEW.worker_id, OLD.worker_id),
                -- error_code persists on a retried job as "last error", so it
                -- is only attributed to the transition that it caused.
                CASE
                    WHEN NEW.cache_hit THEN 'cache hit'
                    WHEN NEW.state = 'failed'
                      OR (OLD.state = 'running' AND NEW.state = 'queued') THEN NEW.error_code
                END);
    END IF;
    RETURN NULL;
END $$;

CREATE TRIGGER jobs_audit AFTER INSERT OR UPDATE OF state ON jobs
    FOR EACH ROW EXECUTE FUNCTION jobs_record_transition();

-- Replays return the resource's current representation with the original
-- status code. Rows older than 24 h are purged by the reaper.
CREATE TABLE idempotency_keys (
    tenant_id     uuid NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    key           text NOT NULL CHECK (length(key) BETWEEN 1 AND 255),
    request_hash  text NOT NULL,
    resource_type text NOT NULL CHECK (resource_type IN ('job', 'sweep')),
    resource_id   uuid NOT NULL,
    status_code   integer NOT NULL,
    created_at    timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (tenant_id, key)
);
CREATE INDEX idempotency_created_idx ON idempotency_keys (created_at);

CREATE TABLE workers (
    id           uuid PRIMARY KEY,
    hostname     text NOT NULL,
    pid          integer NOT NULL,
    solver_id    text NOT NULL,
    version      text NOT NULL,
    concurrency  integer NOT NULL CHECK (concurrency > 0),
    started_at   timestamptz NOT NULL DEFAULT now(),
    heartbeat_at timestamptz NOT NULL DEFAULT now(),
    stopped_at   timestamptz
);
CREATE INDEX workers_live_idx ON workers (heartbeat_at) WHERE stopped_at IS NULL;
