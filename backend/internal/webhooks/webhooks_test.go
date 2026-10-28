package webhooks_test

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/IshaanS0112/AquaSPH/backend/internal/testutil"
	"github.com/IshaanS0112/AquaSPH/backend/internal/webhooks"
	"github.com/google/uuid"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/prometheus/client_golang/prometheus"
)

var bg = context.Background()

func TestSignatureVerifiesAndRejectsTamperingAndReplays(t *testing.T) {
	secret, err := webhooks.NewSecret()
	if err != nil || !strings.HasPrefix(secret, "whsec_") {
		t.Fatalf("secret %q %v", secret, err)
	}
	body := []byte(`{"id":"e1","type":"job.completed"}`)
	now := time.Unix(1_800_000_000, 0)
	h := webhooks.Sign(secret, now, body)
	if err := webhooks.Verify(secret, h, body, now.Add(time.Minute), 5*time.Minute); err != nil {
		t.Fatalf("valid signature rejected: %v", err)
	}
	if err := webhooks.Verify(secret, h, []byte(`{"id":"e1","type":"job.failed"}`), now, 5*time.Minute); !errors.Is(err, webhooks.ErrBadSignature) {
		t.Fatalf("tampered body: %v", err)
	}
	if err := webhooks.Verify("whsec_other", h, body, now, 5*time.Minute); !errors.Is(err, webhooks.ErrBadSignature) {
		t.Fatalf("wrong secret: %v", err)
	}
	if err := webhooks.Verify(secret, h, body, now.Add(time.Hour), 5*time.Minute); !errors.Is(err, webhooks.ErrStale) {
		t.Fatalf("replayed an hour later: %v", err)
	}
	if err := webhooks.Verify(secret, "v1=abc", body, now, time.Minute); !errors.Is(err, webhooks.ErrBadSignature) {
		t.Fatalf("malformed header: %v", err)
	}
}

func TestURLValidation(t *testing.T) {
	for raw, ok := range map[string]bool{
		"https://hooks.example.com/aquasph":        true,
		"https://hooks.example.com:8443/x?y=1":     true,
		"http://hooks.example.com/aquasph":         false, // https only
		"https://user:pass@hooks.example.com/":     false,
		"https://localhost/hook":                   false,
		"https://api.localhost/hook":               false,
		"https://127.0.0.1/hook":                   false,
		"https://169.254.169.254/latest/meta-data": false, // cloud metadata
		"https://10.0.0.5/hook":                    false,
		"https://[::1]/hook":                       false,
		"https://[::ffff:10.0.0.1]/hook":           false, // IPv4-mapped private
		"https://100.64.0.1/hook":                  false, // CGNAT
		"ftp://hooks.example.com/":                 false,
		"/relative":                                false,
		"https://hooks.example.com/#frag":          false,
	} {
		if err := webhooks.ValidateURL(raw, false); (err == nil) != ok {
			t.Errorf("%s: got err=%v, want ok=%v", raw, err, ok)
		}
	}
	if err := webhooks.ValidateURL("http://127.0.0.1:9000/hook", true); err != nil {
		t.Errorf("insecure mode should allow a local http receiver: %v", err)
	}
}

// The registration check sees only the name; this is the check that sees where it resolves.
func TestGuardRefusesPrivateAddressesAtDialTime(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {}))
	defer srv.Close()
	_, err := webhooks.NewHTTPClient(time.Second, false).Get(srv.URL)
	if !errors.Is(err, webhooks.ErrBlockedAddress) {
		t.Fatalf("loopback target reached: %v", err)
	}
	res, err := webhooks.NewHTTPClient(time.Second, true).Get(srv.URL)
	if err != nil {
		t.Fatalf("allow-private client: %v", err)
	}
	res.Body.Close()
}

func TestGuardDoesNotFollowRedirects(t *testing.T) {
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		t.Error("redirect was followed")
	}))
	defer target.Close()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, target.URL, http.StatusFound)
	}))
	defer srv.Close()
	res, err := webhooks.NewHTTPClient(time.Second, true).Post(srv.URL, "application/json", nil)
	if err != nil {
		t.Fatal(err)
	}
	res.Body.Close()
	if res.StatusCode != http.StatusFound {
		t.Fatalf("status %d", res.StatusCode)
	}
}

func TestBackoffGrowsWithJitterAndIsCapped(t *testing.T) {
	d := webhooks.NewDispatcher(nil, webhooks.Config{BackoffBase: 10 * time.Second, BackoffMax: time.Minute}, nil, nil)
	for attempt, want := range map[int]time.Duration{1: 10 * time.Second, 2: 20 * time.Second, 3: 40 * time.Second, 9: time.Minute} {
		for range 50 {
			got := d.Backoff(attempt)
			if got < want/2 || got > want {
				t.Fatalf("attempt %d: %s outside [%s, %s]", attempt, got, want/2, want)
			}
		}
	}
}

// --- against Postgres ---

type fixture struct {
	t      *testing.T
	pool   *pgxpool.Pool
	st     *store.Store
	tenant uuid.UUID
}

func newFixture(t *testing.T) *fixture {
	pool, _ := testutil.NewDB(t)
	st := store.New(pool)
	tn, err := st.CreateTenant(bg, testutil.Unique("t"), store.TenantLimits{})
	if err != nil {
		t.Fatal(err)
	}
	return &fixture{t: t, pool: pool, st: st, tenant: tn.ID}
}

func (f *fixture) endpoint(tenant uuid.UUID, url string, types ...string) *store.WebhookEndpoint {
	f.t.Helper()
	secret, _ := webhooks.NewSecret()
	ep, err := f.st.CreateWebhookEndpoint(bg, tenant, url, "", types, secret)
	if err != nil {
		f.t.Fatal(err)
	}
	return ep
}

func (f *fixture) job(tenant uuid.UUID) uuid.UUID {
	f.t.Helper()
	spec := json.RawMessage(`{"name":"dam_break"}`)
	j, err := store.InsertJob(bg, f.pool, &store.NewJob{ID: ids.New(), TenantID: tenant, Priority: 5, Scenario: "dam_break",
		Quality: "low", MaxParticles: 1000, TimeoutSeconds: 60, Spec: spec, SpecHash: ids.New().String(), AllowCache: true})
	if err != nil {
		f.t.Fatal(err)
	}
	return j.ID
}

// finish walks a job through running into a terminal state, satisfying the table's invariants.
func (f *fixture) finish(id uuid.UUID, state string) {
	f.t.Helper()
	f.exec(`UPDATE jobs SET state = 'running', attempt = 1, lease_token = gen_random_uuid(),
		lease_expires_at = now() + interval '1 minute', started_at = now() WHERE id = $1`, id)
	switch state {
	case "completed":
		f.exec(`UPDATE jobs SET state = 'completed', outcome = 'stable', lease_token = NULL, lease_expires_at = NULL,
			finished_at = now() WHERE id = $1`, id)
	case "failed":
		f.exec(`UPDATE jobs SET state = 'failed', error_code = 'solver_crashed', lease_token = NULL,
			lease_expires_at = NULL, finished_at = now() WHERE id = $1`, id)
	}
}

func (f *fixture) exec(sql string, args ...any) {
	f.t.Helper()
	if _, err := f.pool.Exec(bg, sql, args...); err != nil {
		f.t.Fatal(err)
	}
}

func (f *fixture) count(sql string, args ...any) int {
	f.t.Helper()
	var n int
	if err := f.pool.QueryRow(bg, sql, args...).Scan(&n); err != nil {
		f.t.Fatal(err)
	}
	return n
}

func TestOutboxRecordsTerminalTransitionsForSubscribedEndpointsOnly(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	onCompleted := f.endpoint(f.tenant, "https://a.example.com/hook", "job.completed")
	onFailed := f.endpoint(f.tenant, "https://b.example.com/hook", "job.failed")
	other, _ := f.st.CreateTenant(bg, testutil.Unique("o"), store.TenantLimits{})
	stranger := f.endpoint(other.ID, "https://c.example.com/hook", "job.completed", "job.failed")

	a := f.job(f.tenant)
	f.finish(a, "completed")
	b := f.job(f.tenant)
	f.finish(b, "failed")

	// queued and running are not events; each terminal transition is exactly one.
	if n := f.count(`SELECT count(*) FROM outbox_events WHERE tenant_id = $1`, f.tenant); n != 2 {
		t.Fatalf("events %d, want 2", n)
	}
	if n := f.count(`SELECT count(*) FROM outbox_events WHERE job_id = $1 AND type = 'job.completed'`, a); n != 1 {
		t.Fatalf("completed events for a: %d", n)
	}
	for ep, want := range map[uuid.UUID]int{onCompleted.ID: 1, onFailed.ID: 1, stranger.ID: 0} {
		if n := f.count(`SELECT count(*) FROM webhook_deliveries WHERE endpoint_id = $1`, ep); n != want {
			t.Errorf("endpoint %s: %d deliveries, want %d", ep, n, want)
		}
	}
	var payload map[string]any
	f.pool.QueryRow(bg, `SELECT payload FROM outbox_events WHERE job_id = $1`, b).Scan(&payload)
	if payload["error_code"] != "solver_crashed" || payload["state"] != "failed" {
		t.Fatalf("payload %v", payload)
	}
}

func TestOutboxEventsRollBackWithTheirTransition(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.endpoint(f.tenant, "https://a.example.com/hook", "job.completed")
	id := f.job(f.tenant)
	f.exec(`UPDATE jobs SET state = 'running', attempt = 1, lease_token = gen_random_uuid(),
		lease_expires_at = now() + interval '1 minute' WHERE id = $1`, id)
	tx, _ := f.pool.Begin(bg)
	tx.Exec(bg, `UPDATE jobs SET state = 'completed', outcome = 'stable', lease_token = NULL, lease_expires_at = NULL,
		finished_at = now() WHERE id = $1`, id)
	tx.Rollback(bg)
	if n := f.count(`SELECT count(*) FROM outbox_events`); n != 0 {
		t.Fatalf("%d events survived a rolled-back transition", n)
	}
}

type receiver struct {
	mu       sync.Mutex
	statuses []int // responses to give, in order; the last one repeats
	got      []*http.Request
	bodies   [][]byte
	srv      *httptest.Server
}

func newReceiver(t *testing.T, statuses ...int) *receiver {
	r := &receiver{statuses: statuses}
	r.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		b, _ := io.ReadAll(req.Body)
		r.mu.Lock()
		defer r.mu.Unlock()
		r.got = append(r.got, req)
		r.bodies = append(r.bodies, b)
		status := r.statuses[min(len(r.got)-1, len(r.statuses)-1)]
		w.WriteHeader(status)
	}))
	t.Cleanup(r.srv.Close)
	return r
}

func dispatcher(f *fixture, cfg webhooks.Config) *webhooks.Dispatcher {
	cfg.AllowPrivate = true // receivers are httptest servers on loopback
	return webhooks.NewDispatcher(f.pool, cfg, webhooks.NewMetrics(prometheusRegistry()), testutil.Logger(f.t))
}

func TestDispatcherDeliversSignedEventsOnce(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	rcv := newReceiver(t, 204)
	secret, _ := webhooks.NewSecret()
	ep, _ := f.st.CreateWebhookEndpoint(bg, f.tenant, rcv.srv.URL+"/hook", "", []string{"job.completed"}, secret)
	id := f.job(f.tenant)
	f.finish(id, "completed")

	d := dispatcher(f, webhooks.Config{})
	if n, err := d.DeliverDue(bg); n != 1 || err != nil {
		t.Fatalf("delivered %d: %v", n, err)
	}
	if n, _ := d.DeliverDue(bg); n != 0 {
		t.Fatalf("a delivered event was claimed again (%d)", n)
	}
	if len(rcv.got) != 1 {
		t.Fatalf("receiver saw %d requests", len(rcv.got))
	}
	req, body := rcv.got[0], rcv.bodies[0]
	if err := webhooks.Verify(secret, req.Header.Get(webhooks.SignatureHeader), body, time.Now(), time.Minute); err != nil {
		t.Fatalf("signature: %v", err)
	}
	var env webhooks.Envelope
	if err := json.Unmarshal(body, &env); err != nil {
		t.Fatal(err)
	}
	var data struct {
		Job map[string]any `json:"job"`
	}
	json.Unmarshal(env.Data, &data)
	if env.Type != "job.completed" || req.Header.Get("AquaSPH-Event-Id") != env.ID.String() ||
		data.Job["id"] != id.String() || data.Job["outcome"] != "stable" {
		t.Fatalf("envelope %+v data %v headers %v", env, data, req.Header)
	}
	ds, _ := f.st.ListWebhookDeliveries(bg, f.tenant, ep.ID, 10)
	if len(ds) != 1 || ds[0].State != "delivered" || *ds[0].LastStatus != 204 || ds[0].Attempt != 1 {
		t.Fatalf("delivery %+v", ds[0])
	}
}

func TestFailingReceiverIsRetriedWithBackoffThenDeadLettered(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	rcv := newReceiver(t, 500)
	ep := f.endpoint(f.tenant, rcv.srv.URL, "job.failed")
	f.finish(f.job(f.tenant), "failed")

	d := dispatcher(f, webhooks.Config{MaxAttempts: 3, BackoffBase: 200 * time.Millisecond, BackoffMax: 200 * time.Millisecond})
	d.DeliverDue(bg)
	ds, _ := f.st.ListWebhookDeliveries(bg, f.tenant, ep.ID, 10)
	if ds[0].State != "pending" || ds[0].Attempt != 1 || ds[0].NextAttemptAt == nil || *ds[0].LastStatus != 500 {
		t.Fatalf("after one failure: %+v", ds[0])
	}
	// Not due yet: backoff is honoured.
	if n, _ := d.DeliverDue(bg); n != 0 {
		t.Fatal("retried before its backoff elapsed")
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		d.DeliverDue(bg)
		if ds, _ = f.st.ListWebhookDeliveries(bg, f.tenant, ep.ID, 10); ds[0].State == "dead" {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if ds[0].State != "dead" || ds[0].Attempt != 3 || len(rcv.got) != 3 {
		t.Fatalf("after exhausting attempts: %+v (%d requests)", ds[0], len(rcv.got))
	}

	// Manual redrive puts it back, and a receiver that has recovered gets it.
	rcv.mu.Lock()
	rcv.statuses = []int{200}
	rcv.mu.Unlock()
	if err := f.st.RedeliverWebhook(bg, f.tenant, ep.ID, ds[0].ID); err != nil {
		t.Fatal(err)
	}
	d.DeliverDue(bg)
	ds, _ = f.st.ListWebhookDeliveries(bg, f.tenant, ep.ID, 10)
	if ds[0].State != "delivered" {
		t.Fatalf("after redrive: %+v", ds[0])
	}
}

// A dispatcher that died mid-request leaves a 'delivering' row; it must not be stuck there.
func TestExpiredLeaseIsReclaimed(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	rcv := newReceiver(t, 200)
	ep := f.endpoint(f.tenant, rcv.srv.URL, "job.completed")
	f.finish(f.job(f.tenant), "completed")
	f.exec(`UPDATE webhook_deliveries SET state = 'delivering', attempt = 1,
		lease_expires_at = now() - interval '1 second' WHERE endpoint_id = $1`, ep.ID)

	d := dispatcher(f, webhooks.Config{})
	if n, _ := d.DeliverDue(bg); n != 1 {
		t.Fatalf("claimed %d", n)
	}
	ds, _ := f.st.ListWebhookDeliveries(bg, f.tenant, ep.ID, 10)
	if ds[0].State != "delivered" || ds[0].Attempt != 2 {
		t.Fatalf("%+v", ds[0])
	}
	if got := rcv.got[0].Header.Get("AquaSPH-Delivery-Attempt"); got != "2" {
		t.Fatalf("attempt header %q", got)
	}
}

func TestBlockedTargetIsRecordedAsTheFailureReason(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	rcv := newReceiver(t, 200)
	ep := f.endpoint(f.tenant, rcv.srv.URL, "job.completed")
	f.finish(f.job(f.tenant), "completed")
	d := webhooks.NewDispatcher(f.pool, webhooks.Config{}, nil, testutil.Logger(t)) // guard on
	d.DeliverDue(bg)
	ds, _ := f.st.ListWebhookDeliveries(bg, f.tenant, ep.ID, 10)
	if ds[0].State != "pending" || ds[0].LastError == nil || *ds[0].LastError != webhooks.ErrBlockedAddress.Error() {
		t.Fatalf("%+v", ds[0])
	}
	if len(rcv.got) != 0 {
		t.Fatal("a loopback receiver was reached with the guard on")
	}
}

func TestEndpointLimitHoldsUnderConcurrentCreation(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	var wg sync.WaitGroup
	var mu sync.Mutex
	created := 0
	for range 20 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if _, err := f.st.CreateWebhookEndpoint(bg, f.tenant, "https://x.example.com", "", []string{"job.completed"}, "s"); err == nil {
				mu.Lock()
				created++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if created != store.MaxWebhookEndpoints {
		t.Fatalf("created %d endpoints, limit %d", created, store.MaxWebhookEndpoints)
	}
}

func prometheusRegistry() *prometheus.Registry { return prometheus.NewRegistry() }
