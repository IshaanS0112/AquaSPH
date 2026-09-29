package api_test

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	apispec "github.com/IshaanS0112/AquaSPH/backend/api"
	"github.com/IshaanS0112/AquaSPH/backend/internal/api"
	"github.com/IshaanS0112/AquaSPH/backend/internal/artifacts"
	"github.com/IshaanS0112/AquaSPH/backend/internal/config"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/events"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/obs"
	"github.com/IshaanS0112/AquaSPH/backend/internal/queue"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ratelimit"
	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/IshaanS0112/AquaSPH/backend/internal/testutil"
	"github.com/google/uuid"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	promtest "github.com/prometheus/client_golang/prometheus/testutil"
	"github.com/redis/go-redis/v9"
)

var bg = context.Background()

type env struct {
	t        *testing.T
	srv      *httptest.Server
	internal *httptest.Server
	st       *store.Store
	q        *queue.Queue
	art      *artifacts.LocalFS
	metrics  *obs.APIMetrics
	tenant   *domain.Tenant
	key      string
}

type opts struct {
	redisURL string // "" = no Redis; "down" = a Redis that is not there
	ssePoll  time.Duration
	cache    config.CacheScope
}

func newEnv(t *testing.T, o opts) *env {
	t.Helper()
	pool, _ := testutil.NewDB(t)
	st := store.New(pool)
	cat, err := scenario.LoadCatalog("../../../configs/scenarios")
	if err != nil {
		t.Fatal(err)
	}
	art, _ := artifacts.NewLocalFS(t.TempDir())
	reg := obs.NewRegistry()
	m := obs.NewAPIMetrics(reg)
	if o.cache == "" {
		o.cache = config.CacheTenant
	}
	d := api.Deps{
		Store: st, Catalog: cat, Artifacts: art, Metrics: m, Log: testutil.Logger(t),
		Service: service.New(st, cat, service.Config{CacheScope: o.cache, LiveWorkerWindow: time.Minute, MaxSweepSize: 64}),
		Ready: func(ctx context.Context) (map[string]string, bool) {
			if err := pool.Ping(ctx); err != nil {
				return map[string]string{"postgres": err.Error()}, false
			}
			return map[string]string{"postgres": "ok"}, true
		},
		MaxBodyBytes: 64 << 10, SSEPollInterval: o.ssePoll, SSEPingInterval: 50 * time.Millisecond,
	}
	switch o.redisURL {
	case "":
	case "down":
		c := redis.NewClient(&redis.Options{Addr: "127.0.0.1:1", DialTimeout: 50 * time.Millisecond, MaxRetries: -1})
		t.Cleanup(func() { c.Close() })
		d.Limiter = ratelimit.NewRedis(c, "t:")
	default:
		ro, _ := redis.ParseURL(o.redisURL)
		c := redis.NewClient(ro)
		t.Cleanup(func() { c.Close() })
		d.Limiter = ratelimit.NewRedis(c, "t:"+ids.New().String()+":")
		d.Subscriber = events.NewRedis(c)
	}
	s := api.New(d)
	e := &env{t: t, st: st, art: art, metrics: m,
		q:        queue.New(pool, queue.Config{Lease: 30 * time.Second}),
		srv:      httptest.NewServer(s.Handler()),
		internal: httptest.NewServer(s.InternalHandler(promhttp.HandlerFor(reg, promhttp.HandlerOpts{}))),
	}
	t.Cleanup(e.srv.Close)
	t.Cleanup(e.internal.Close)
	e.tenant, e.key = e.newTenant(store.TenantLimits{})
	return e
}

func (e *env) newTenant(l store.TenantLimits) (*domain.Tenant, string) {
	e.t.Helper()
	tn, err := e.st.CreateTenant(bg, testutil.Unique("t"), l)
	if err != nil {
		e.t.Fatal(err)
	}
	key, _, err := e.st.CreateAPIKey(bg, tn.ID, "test")
	if err != nil {
		e.t.Fatal(err)
	}
	return tn, key
}

type resp struct {
	*http.Response
	body []byte
}

func (r resp) json(t *testing.T) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(r.body, &m); err != nil {
		t.Fatalf("not JSON (%d): %s", r.StatusCode, r.body)
	}
	return m
}

func (e *env) req(method, path, key string, body any, hdr ...string) resp {
	e.t.Helper()
	var rd io.Reader
	switch b := body.(type) {
	case nil:
	case string:
		rd = strings.NewReader(b)
	default:
		buf, _ := json.Marshal(b)
		rd = bytes.NewReader(buf)
	}
	r, _ := http.NewRequest(method, e.srv.URL+path, rd)
	if key != "" {
		r.Header.Set("Authorization", "Bearer "+key)
	}
	if body != nil {
		r.Header.Set("Content-Type", "application/json")
	}
	for i := 0; i+1 < len(hdr); i += 2 {
		r.Header.Set(hdr[i], hdr[i+1])
	}
	res, err := http.DefaultClient.Do(r)
	if err != nil {
		e.t.Fatal(err)
	}
	defer res.Body.Close()
	b, _ := io.ReadAll(res.Body)
	return resp{res, b}
}

func (e *env) do(method, path string, body any, hdr ...string) resp {
	return e.req(method, path, e.key, body, hdr...)
}

func expect(t *testing.T, r resp, status int) {
	t.Helper()
	if r.StatusCode != status {
		t.Fatalf("status %d, want %d: %s", r.StatusCode, status, r.body)
	}
}

var damBreak = map[string]any{"scenario": "dam_break", "quality": "low", "sim_time": 0.1}

// The spec and the router must describe the same API.
func TestEveryRouteIsDocumentedAndEveryDocumentedRouteExists(t *testing.T) {
	documented := map[string]bool{}
	var path string
	inPaths := false
	for _, line := range strings.Split(string(apispec.OpenAPI), "\n") {
		switch {
		case line == "paths:":
			inPaths = true
		case inPaths && len(line) > 0 && line[0] != ' ':
			inPaths = false
		case inPaths && strings.HasPrefix(line, "  /") && strings.HasSuffix(line, ":"):
			path = strings.TrimSuffix(strings.TrimSpace(line), ":")
		case inPaths && path != "" && len(line) > 4 && line[:4] == "    " && line[4] != ' ':
			method := strings.TrimSuffix(strings.TrimSpace(line), ":")
			switch method {
			case "get", "post", "put", "patch", "delete":
				documented[strings.ToUpper(method)+" "+path] = true
			}
		}
	}
	registered := map[string]bool{}
	for _, p := range api.RoutePatterns() {
		registered[p] = true
		if !documented[p] {
			t.Errorf("route %q is not in api/openapi.yaml", p)
		}
	}
	for p := range documented {
		if !registered[p] {
			t.Errorf("api/openapi.yaml documents %q, which is not a route", p)
		}
	}
}

func TestAuthentication(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	for name, key := range map[string]string{
		"missing":   "",
		"malformed": "not-a-key",
		"unknown":   "aqk_zzzzzzzz_" + strings.Repeat("A", 32),
		"wrong":     e.key[:len(e.key)-4] + "AAAA",
	} {
		r := e.req("GET", "/v1/jobs", key, nil)
		if r.StatusCode != 401 || r.Header.Get("WWW-Authenticate") == "" ||
			r.Header.Get("Content-Type") != "application/problem+json" {
			t.Errorf("%s key: %d %s", name, r.StatusCode, r.body)
		}
	}
	expect(t, e.do("GET", "/v1/jobs", nil), 200)
	prefix := strings.Split(e.key, "_")[1]
	e.st.RevokeAPIKey(bg, prefix)
	if r := e.do("GET", "/v1/jobs", nil); r.StatusCode != 401 || !strings.Contains(string(r.body), "revoked") {
		t.Fatalf("revoked key: %d %s", r.StatusCode, r.body)
	}
}

func TestJobLifecycleOverHTTP(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	r := e.do("POST", "/v1/jobs", damBreak)
	expect(t, r, 201)
	job := r.json(t)
	id := job["id"].(string)
	if r.Header.Get("Location") != "/v1/jobs/"+id || job["state"] != "queued" || job["priority"] != 5.0 {
		t.Fatalf("%v %s", r.Header, r.body)
	}
	if _, leaked := job["lease_token"]; leaked {
		t.Fatal("internal field in the API representation")
	}
	expect(t, e.do("GET", "/v1/jobs/"+id, nil), 200)
	if g := e.do("GET", "/v1/jobs/"+id+"?include=spec", nil).json(t); g["spec"] == nil {
		t.Fatal("include=spec did not include the spec")
	}
	if l := e.do("GET", "/v1/jobs?state=queued", nil).json(t); len(l["data"].([]any)) != 1 {
		t.Fatalf("list: %v", l)
	}
	expect(t, e.do("GET", "/v1/jobs/"+id+"/result", nil), 409)

	c := e.do("POST", "/v1/jobs/"+id+"/cancel", nil)
	expect(t, c, 202)
	if c.json(t)["state"] != "cancelled" {
		t.Fatalf("%s", c.body)
	}
	expect(t, e.do("POST", "/v1/jobs/"+id+"/cancel", nil), 409)
	h := e.do("GET", "/v1/jobs/"+id+"/history", nil).json(t)
	if evs := h["events"].([]any); len(evs) != 2 || evs[1].(map[string]any)["to_state"] != "cancelled" {
		t.Fatalf("history %v", h)
	}
}

func TestValidationErrorsListEveryField(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	r := e.do("POST", "/v1/jobs", map[string]any{"scenario": "dam_break", "quality": "ultra",
		"overrides": map[string]any{"/numerics/xsph": 1.0}})
	expect(t, r, 422)
	p := r.json(t)
	if p["type"] != "/problems/validation-error" || len(p["errors"].([]any)) != 2 || p["request_id"] == "" {
		t.Fatalf("%s", r.body)
	}
}

func TestStrictDecoding(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	r := e.do("POST", "/v1/jobs", `{"scenario":"dam_break","sim_tme":0.5}`)
	if r.StatusCode != 400 || !strings.Contains(string(r.body), "unknown field") {
		t.Fatalf("typo field: %d %s", r.StatusCode, r.body)
	}
	expect(t, e.do("POST", "/v1/jobs", `{"scenario":"dam_break"} {"again":1}`), 400)
	expect(t, e.do("POST", "/v1/jobs", `{`), 400)
	big := `{"scenario":"dam_break","labels":{"x":"` + strings.Repeat("a", 70<<10) + `"}}`
	expect(t, e.do("POST", "/v1/jobs", big), 413)
	expect(t, e.do("POST", "/v1/jobs", "scenario=dam_break", "Content-Type", "application/x-www-form-urlencoded"), 415)
}

func TestIdempotencyKeyReplaysOverHTTP(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	a := e.do("POST", "/v1/jobs", damBreak, "Idempotency-Key", "abc")
	b := e.do("POST", "/v1/jobs", damBreak, "Idempotency-Key", "abc")
	expect(t, a, 201)
	expect(t, b, 201)
	if a.json(t)["id"] != b.json(t)["id"] || b.Header.Get("Idempotent-Replayed") != "true" {
		t.Fatalf("not replayed: %s / %s", a.body, b.body)
	}
	other := map[string]any{"scenario": "dam_break", "quality": "medium"}
	expect(t, e.do("POST", "/v1/jobs", other, "Idempotency-Key", "abc"), 422)
}

func TestRateLimitReturns429WithHeaders(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{redisURL: testutil.RedisURL(t)})
	rate, burst := 60, 2
	_, key := e.newTenant(store.TenantLimits{RatePerMinute: &rate, RateBurst: &burst})
	for i := 0; i < 2; i++ {
		r := e.req("GET", "/v1/jobs", key, nil)
		expect(t, r, 200)
		if r.Header.Get("RateLimit-Limit") != "2" {
			t.Fatalf("headers %v", r.Header)
		}
	}
	r := e.req("GET", "/v1/jobs", key, nil)
	if r.StatusCode != 429 || r.Header.Get("Retry-After") == "" || r.json(t)["type"] != "/problems/rate-limited" {
		t.Fatalf("%d %v %s", r.StatusCode, r.Header, r.body)
	}
	expect(t, e.do("GET", "/v1/jobs", nil), 200) // another tenant is unaffected
}

func TestRateLimiterFailsOpenWhenRedisIsDown(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{redisURL: "down"})
	for i := 0; i < 3; i++ {
		expect(t, e.do("GET", "/v1/jobs", nil), 200)
	}
	if n := promtest.ToFloat64(e.metrics.RateLimitErrors); n != 3 {
		t.Fatalf("fail-open not counted: %v", n)
	}
}

func TestOtherTenantsJobsAre404Everywhere(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	id := e.do("POST", "/v1/jobs", damBreak).json(t)["id"].(string)
	_, other := e.newTenant(store.TenantLimits{})
	for _, p := range []string{"", "/result", "/history", "/artifacts", "/artifacts/metrics.json", "/events"} {
		if r := e.req("GET", "/v1/jobs/"+id+p, other, nil); r.StatusCode != 404 {
			t.Errorf("GET %s: %d", p, r.StatusCode)
		}
	}
	if r := e.req("POST", "/v1/jobs/"+id+"/cancel", other, nil); r.StatusCode != 404 {
		t.Errorf("cancel: %d", r.StatusCode)
	}
}

func TestRoutingErrorsAreProblemJSON(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	expect(t, e.do("GET", "/v1/jobs/not-a-uuid", nil), 404)
	r := e.do("GET", "/v1/nope", nil)
	if r.StatusCode != 404 || r.Header.Get("Content-Type") != "application/problem+json" {
		t.Fatalf("%d %s", r.StatusCode, r.body)
	}
	r = e.do("DELETE", "/v1/jobs", nil)
	if r.StatusCode != 405 || r.Header.Get("Allow") == "" || r.Header.Get("Content-Type") != "application/problem+json" {
		t.Fatalf("%d %v", r.StatusCode, r.Header)
	}
}

func TestRequestIDIsEchoedOrGenerated(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	if r := e.do("GET", "/v1/jobs", nil, "X-Request-ID", "trace-123"); r.Header.Get("X-Request-ID") != "trace-123" {
		t.Fatal("request ID not echoed")
	}
	if r := e.do("GET", "/v1/jobs", nil, "X-Request-ID", "bad id with spaces"); len(r.Header.Get("X-Request-ID")) != 24 {
		t.Fatal("invalid request ID not replaced")
	}
}

// runJob pushes a queued job through the queue as a worker would, with a
// real artifact on disk.
func (e *env) runJob(id string, metrics string) {
	e.t.Helper()
	w := ids.New()
	j, err := e.q.Claim(bg, w)
	if err != nil || j == nil || j.ID.String() != id {
		e.t.Fatalf("claim %v %v", j, err)
	}
	a, _ := e.art.Put(bg, j.ID, "metrics.json", strings.NewReader(metrics))
	arts, _ := json.Marshal([]domain.Artifact{a})
	if err := e.q.Complete(bg, j.ID, *j.LeaseToken, queue.Completion{Outcome: "stable", Result: json.RawMessage(metrics),
		Artifacts: arts, SolverID: "s", CacheKey: scenario.CacheKey(j.SpecHash, "s")}); err != nil {
		e.t.Fatal(err)
	}
}

func TestResultAndArtifactDownload(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	id := e.do("POST", "/v1/jobs", damBreak).json(t)["id"].(string)
	body := `{"status":"STABLE","time":{"steps":42}}`
	e.runJob(id, body)

	res := e.do("GET", "/v1/jobs/"+id+"/result", nil).json(t)
	if res["partial"] != false || res["metrics"].(map[string]any)["status"] != "STABLE" {
		t.Fatalf("%v", res)
	}
	list := e.do("GET", "/v1/jobs/"+id+"/artifacts", nil).json(t)["data"].([]any)
	if len(list) != 1 || list[0].(map[string]any)["url"] != "/v1/jobs/"+id+"/artifacts/metrics.json" {
		t.Fatalf("%v", list)
	}
	f := e.do("GET", "/v1/jobs/"+id+"/artifacts/metrics.json", nil)
	expect(t, f, 200)
	if string(f.body) != body || f.Header.Get("Content-Type") != "application/json" || f.Header.Get("ETag") == "" {
		t.Fatalf("%v %s", f.Header, f.body)
	}
	expect(t, e.do("GET", "/v1/jobs/"+id+"/artifacts/metrics.json", nil, "If-None-Match", f.Header.Get("ETag")), 304)
	part := e.do("GET", "/v1/jobs/"+id+"/artifacts/metrics.json", nil, "Range", "bytes=0-9")
	if part.StatusCode != 206 || string(part.body) != body[:10] {
		t.Fatalf("range: %d %q", part.StatusCode, part.body)
	}
	for _, name := range []string{"stderr.log", "..%2F..%2Fetc%2Fpasswd", "%2e%2e"} {
		if r := e.do("GET", "/v1/jobs/"+id+"/artifacts/"+name, nil); r.StatusCode != 404 {
			t.Errorf("%s: %d", name, r.StatusCode)
		}
	}
}

func TestSecondIdenticalSubmissionIsACacheHit(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	e.st.RegisterWorker(bg, &domain.Worker{ID: ids.New(), Hostname: "h", PID: 1, SolverID: "s", Version: "t", Concurrency: 1})
	id := e.do("POST", "/v1/jobs", damBreak).json(t)["id"].(string)
	e.runJob(id, `{"status":"STABLE"}`)
	hit := e.do("POST", "/v1/jobs", damBreak).json(t)
	if hit["state"] != "completed" || hit["cache_hit"] != true || hit["source_job_id"] != id {
		t.Fatalf("%v", hit)
	}
	// The hit's artifacts are the source's files.
	expect(t, e.do("GET", "/v1/jobs/"+hit["id"].(string)+"/artifacts/metrics.json", nil), 200)
}

func TestSweepOverHTTPIncludingCSV(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	r := e.do("POST", "/v1/sweeps", map[string]any{
		"scenario": "dam_break", "quality": "low", "sim_time": 0.1,
		"grid": map[string]any{"/materials/0/viscosity": []any{-0.5, 1.0}, "/materials/0/name": []any{"=HYPERLINK(1)", "water"}},
	})
	expect(t, r, 201)
	sw := r.json(t)
	if sw["total"] != 4.0 || sw["state"] != "running" || sw["counts"].(map[string]any)["queued"] != 4.0 {
		t.Fatalf("%s", r.body)
	}
	id := sw["id"].(string)
	if l := e.do("GET", "/v1/jobs?sweep_id="+id, nil).json(t); len(l["data"].([]any)) != 4 {
		t.Fatalf("children: %v", l)
	}
	csvResp := e.do("GET", "/v1/sweeps/"+id+"/results?format=csv&metrics=/status", nil)
	expect(t, csvResp, 200)
	lines := strings.Split(strings.TrimSpace(string(csvResp.body)), "\n")
	if len(lines) != 5 || lines[0] != "job_id,state,outcome,cache_hit,/materials/0/name,/materials/0/viscosity,/status" {
		t.Fatalf("csv:\n%s", csvResp.body)
	}
	if !strings.Contains(lines[1], ",'=HYPERLINK(1),-0.5,") {
		t.Fatalf("formula not neutralised, or a negative number was: %s", lines[1])
	}
	expect(t, e.do("POST", "/v1/sweeps/"+id+"/cancel", nil), 202)
	if g := e.do("GET", "/v1/sweeps/"+id, nil).json(t); g["state"] != "completed" || g["counts"].(map[string]any)["cancelled"] != 4.0 {
		t.Fatalf("%v", g)
	}
}

func readEvents(t *testing.T, body io.Reader, n int, timeout time.Duration) []string {
	t.Helper()
	got := make(chan string, 64)
	go func() {
		sc := bufio.NewScanner(body)
		for sc.Scan() {
			if ev, ok := strings.CutPrefix(sc.Text(), "event: "); ok {
				got <- ev
			}
		}
		close(got)
	}()
	var out []string
	deadline := time.After(timeout)
	for len(out) < n {
		select {
		case ev, ok := <-got:
			if !ok {
				return out
			}
			out = append(out, ev)
		case <-deadline:
			return out
		}
	}
	return out
}

func (e *env) stream(id string) *http.Response {
	e.t.Helper()
	r, _ := http.NewRequest("GET", e.srv.URL+"/v1/jobs/"+id+"/events", nil)
	r.Header.Set("Authorization", "Bearer "+e.key)
	res, err := http.DefaultClient.Do(r)
	if err != nil {
		e.t.Fatal(err)
	}
	e.t.Cleanup(func() { res.Body.Close() })
	if res.StatusCode != 200 || res.Header.Get("Content-Type") != "text/event-stream" {
		e.t.Fatalf("%d %v", res.StatusCode, res.Header)
	}
	return res
}

// Without Redis, SSE still delivers every state change and progress by
// polling Postgres.
func TestSSEWithoutRedisFallsBackToPolling(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{ssePoll: 30 * time.Millisecond})
	id := e.do("POST", "/v1/jobs", damBreak).json(t)["id"].(string)
	res := e.stream(id)
	done := make(chan []string)
	go func() { done <- readEvents(t, res.Body, 6, 10*time.Second) }()

	time.Sleep(100 * time.Millisecond)
	j, _ := e.q.Claim(bg, ids.New())
	time.Sleep(100 * time.Millisecond)
	e.q.Heartbeat(bg, j.ID, *j.LeaseToken, json.RawMessage(`{"t":0.05,"t_end":0.1,"fraction":0.5}`))
	time.Sleep(100 * time.Millisecond)
	e.q.Complete(bg, j.ID, *j.LeaseToken, queue.Completion{Outcome: "stable", Result: json.RawMessage(`{}`), SolverID: "s", CacheKey: "k"})

	got := strings.Join(<-done, ",")
	if got != "snapshot,state,progress,state,done" {
		t.Fatalf("events: %s", got)
	}
}

// With Redis, progress arrives by pub/sub, long before the next poll.
func TestSSEDeliversPublishedProgressImmediately(t *testing.T) {
	t.Parallel()
	url := testutil.RedisURL(t)
	e := newEnv(t, opts{redisURL: url, ssePoll: time.Hour})
	id := e.do("POST", "/v1/jobs", damBreak).json(t)["id"].(string)
	res := e.stream(id)
	done := make(chan []string)
	go func() { done <- readEvents(t, res.Body, 2, 5*time.Second) }()

	ro, _ := redis.ParseURL(url)
	c := redis.NewClient(ro)
	defer c.Close()
	pub := events.NewRedis(c)
	time.Sleep(100 * time.Millisecond)
	jid := mustUUID(t, id)
	pub.Publish(bg, events.Event{Type: "progress", JobID: jid, Progress: &domain.Progress{T: 0.01, Fraction: 0.1}})
	if got := strings.Join(<-done, ","); got != "snapshot,progress" {
		t.Fatalf("events: %s", got)
	}
}

func TestSSEOnFinishedJobSendsSnapshotAndDone(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	id := e.do("POST", "/v1/jobs", damBreak).json(t)["id"].(string)
	e.do("POST", "/v1/jobs/"+id+"/cancel", nil)
	res := e.stream(id)
	if got := strings.Join(readEvents(t, res.Body, 3, 2*time.Second), ","); got != "snapshot,done" {
		t.Fatalf("events: %s", got)
	}
}

func TestProbesOpenAPIAndMetrics(t *testing.T) {
	t.Parallel()
	e := newEnv(t, opts{})
	expect(t, e.req("GET", "/healthz", "", nil), 200)
	if r := e.req("GET", "/readyz", "", nil); r.StatusCode != 200 || r.json(t)["checks"].(map[string]any)["postgres"] != "ok" {
		t.Fatalf("%s", r.body)
	}
	if r := e.req("GET", "/openapi.yaml", "", nil); r.StatusCode != 200 || !bytes.HasPrefix(r.body, []byte("openapi: 3.1.0")) {
		t.Fatal("openapi not served")
	}
	id := e.do("POST", "/v1/jobs", damBreak).json(t)["id"].(string)
	e.do("GET", "/v1/jobs/"+id, nil)

	mr, _ := http.Get(e.internal.URL + "/metrics")
	b, _ := io.ReadAll(mr.Body)
	mr.Body.Close()
	text := string(b)
	for _, want := range []string{
		`aquasph_http_requests_total{code="200",route="GET /v1/jobs/{id}"} 1`,
		`aquasph_submissions_total{kind="job",result="queued"} 1`,
	} {
		if !strings.Contains(text, want) {
			t.Errorf("metrics missing %s", want)
		}
	}
	if strings.Contains(text, id) {
		t.Error("a job ID leaked into a metric label (unbounded cardinality)")
	}
	// /metrics is not on the public port.
	expect(t, e.req("GET", "/metrics", "", nil), 404)
}

func mustUUID(t *testing.T, s string) uuid.UUID {
	t.Helper()
	id, err := parseUUID(s)
	if err != nil {
		t.Fatal(err)
	}
	return id
}

func parseUUID(s string) (uuid.UUID, error) { return uuid.Parse(s) }
