-- Webhooks through a transactional outbox (ADR-0005), and an index for /v1/stats.

-- Where a tenant wants job events delivered. The secret signs every delivery; unlike an API
-- key it has to be stored in a usable form, because the dispatcher signs with it.
CREATE TABLE webhook_endpoints (
    id          uuid PRIMARY KEY,
    tenant_id   uuid NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    url         text NOT NULL CHECK (url ~ '^https?://' AND length(url) <= 2048),
    description text NOT NULL DEFAULT '' CHECK (length(description) <= 200),
    secret      text NOT NULL,
    event_types text[] NOT NULL CHECK (cardinality(event_types) > 0),
    created_at  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX webhook_endpoints_tenant_idx ON webhook_endpoints (tenant_id);

-- The outbox. Rows are written by a trigger in the same transaction as the job transition they
-- describe, so an event exists exactly when the transition committed: no code path can emit an
-- event for a rolled-back change or forget to emit one for a committed change.
CREATE TABLE outbox_events (
    id         uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    tenant_id  uuid NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    type       text NOT NULL,
    job_id     uuid,
    payload    jsonb NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);
CREATE INDEX outbox_events_tenant_idx ON outbox_events (tenant_id, created_at DESC);
CREATE INDEX outbox_events_created_idx ON outbox_events (created_at);

-- One row per (event, endpoint). 'delivering' rows carry a lease, and the attempt number is the
-- fencing token: a dispatcher that stalled past its lease cannot record over a newer attempt.
CREATE TABLE webhook_deliveries (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    event_id         uuid NOT NULL REFERENCES outbox_events(id) ON DELETE CASCADE,
    endpoint_id      uuid NOT NULL REFERENCES webhook_endpoints(id) ON DELETE CASCADE,
    state            text NOT NULL DEFAULT 'pending'
                     CHECK (state IN ('pending', 'delivering', 'delivered', 'dead')),
    attempt          integer NOT NULL DEFAULT 0 CHECK (attempt >= 0),
    next_attempt_at  timestamptz NOT NULL DEFAULT now(),
    lease_expires_at timestamptz,
    last_status      integer,
    last_error       text,
    last_attempt_at  timestamptz,
    delivered_at     timestamptz,
    created_at       timestamptz NOT NULL DEFAULT now(),
    UNIQUE (event_id, endpoint_id),
    CONSTRAINT delivering_has_lease CHECK (state <> 'delivering' OR lease_expires_at IS NOT NULL)
);
CREATE INDEX webhook_deliveries_due_idx   ON webhook_deliveries (next_attempt_at) WHERE state = 'pending';
CREATE INDEX webhook_deliveries_lease_idx ON webhook_deliveries (lease_expires_at) WHERE state = 'delivering';
CREATE INDEX webhook_deliveries_endpoint_idx ON webhook_deliveries (endpoint_id, id DESC);

CREATE FUNCTION jobs_emit_event() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
    ev_type text;
    ev_id   uuid;
BEGIN
    IF NEW.state NOT IN ('completed', 'failed', 'cancelled') THEN
        RETURN NULL;
    END IF;
    IF TG_OP = 'UPDATE' AND OLD.state = NEW.state THEN
        RETURN NULL;
    END IF;
    ev_type := 'job.' || NEW.state::text;
    INSERT INTO outbox_events (tenant_id, type, job_id, payload)
    VALUES (NEW.tenant_id, ev_type, NEW.id, jsonb_build_object(
        'id', NEW.id, 'state', NEW.state, 'outcome', NEW.outcome, 'scenario', NEW.scenario,
        'quality', NEW.quality, 'cache_hit', NEW.cache_hit, 'attempt', NEW.attempt,
        'error_code', NEW.error_code, 'sweep_id', NEW.sweep_id, 'labels', NEW.labels,
        'created_at', NEW.created_at, 'finished_at', NEW.finished_at))
    RETURNING id INTO ev_id;
    INSERT INTO webhook_deliveries (event_id, endpoint_id)
    SELECT ev_id, e.id FROM webhook_endpoints e
    WHERE e.tenant_id = NEW.tenant_id AND ev_type = ANY (e.event_types);
    IF FOUND THEN
        PERFORM pg_notify('aquasph_webhooks', '');
    END IF;
    RETURN NULL;
END $$;

CREATE TRIGGER jobs_outbox AFTER INSERT OR UPDATE OF state ON jobs
    FOR EACH ROW EXECUTE FUNCTION jobs_emit_event();

-- /v1/stats reads a tenant's recent window; without this it scans every job the tenant has.
CREATE INDEX jobs_tenant_finished_idx ON jobs (tenant_id, finished_at) WHERE finished_at IS NOT NULL;
CREATE INDEX jobs_tenant_created_idx  ON jobs (tenant_id, created_at);
