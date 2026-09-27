// Package client is a typed Go client for the AquaSPH HTTP API, used by
// aquactl, the load generator and the end-to-end tests.
package client

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
)

type Client struct {
	BaseURL string
	APIKey  string
	HTTP    *http.Client
}

func New(baseURL, apiKey string) *Client {
	return &Client{BaseURL: strings.TrimRight(baseURL, "/"), APIKey: apiKey,
		HTTP: &http.Client{Timeout: 60 * time.Second}}
}

// APIError is a problem+json response.
type APIError struct {
	Status    int                   `json:"status"`
	Type      string                `json:"type"`
	Title     string                `json:"title"`
	Detail    string                `json:"detail"`
	RequestID string                `json:"request_id"`
	Errors    []scenario.FieldError `json:"errors"`
	// RetryAfter is set on 429s.
	RetryAfter string `json:"-"`
}

func (e *APIError) Error() string {
	var b strings.Builder
	fmt.Fprintf(&b, "%d %s", e.Status, e.Title)
	if e.Detail != "" {
		fmt.Fprintf(&b, ": %s", e.Detail)
	}
	for _, fe := range e.Errors {
		fmt.Fprintf(&b, "\n  %s: %s", fe.Field, fe.Message)
	}
	return b.String()
}

type Job struct {
	ID              string            `json:"id"`
	State           string            `json:"state"`
	Outcome         *string           `json:"outcome"`
	Scenario        string            `json:"scenario"`
	Quality         string            `json:"quality"`
	SimTime         *float64          `json:"sim_time"`
	Priority        int               `json:"priority"`
	Labels          map[string]string `json:"labels"`
	SweepID         *string           `json:"sweep_id"`
	SweepParams     map[string]any    `json:"sweep_params"`
	Progress        *Progress         `json:"progress"`
	Attempt         int               `json:"attempt"`
	MaxAttempts     int               `json:"max_attempts"`
	CancelRequested bool              `json:"cancel_requested"`
	CacheHit        bool              `json:"cache_hit"`
	SourceJobID     *string           `json:"source_job_id"`
	SpecHash        string            `json:"spec_hash"`
	SolverID        *string           `json:"solver_id"`
	Error           *struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
	CreatedAt  time.Time  `json:"created_at"`
	StartedAt  *time.Time `json:"started_at"`
	FinishedAt *time.Time `json:"finished_at"`
}

func (j *Job) Terminal() bool {
	return j.State == "completed" || j.State == "failed" || j.State == "cancelled"
}

type Progress struct {
	T        float64  `json:"t"`
	TEnd     float64  `json:"t_end"`
	Fraction float64  `json:"fraction"`
	Step     int64    `json:"step"`
	Fluid    int64    `json:"fluid"`
	MaxSpeed *float64 `json:"max_speed"`
	WallS    float64  `json:"wall_s"`
}

type Page[T any] struct {
	Data       []T     `json:"data"`
	NextCursor *string `json:"next_cursor"`
}

type Result struct {
	JobID    string          `json:"job_id"`
	State    string          `json:"state"`
	Outcome  *string         `json:"outcome"`
	CacheHit bool            `json:"cache_hit"`
	Partial  bool            `json:"partial"`
	Metrics  json.RawMessage `json:"metrics"`
}

type Artifact struct {
	Name        string `json:"name"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	ContentType string `json:"content_type"`
	URL         string `json:"url"`
}

type Sweep struct {
	ID        string         `json:"id"`
	State     string         `json:"state"`
	Scenario  string         `json:"scenario"`
	Total     int            `json:"total"`
	Counts    map[string]int `json:"counts"`
	CacheHits int            `json:"cache_hits"`
	CreatedAt time.Time      `json:"created_at"`
}

type ScenarioInfo struct {
	Name        string `json:"name"`
	Tier        int    `json:"tier"`
	Description string `json:"description"`
}

// Response is the raw result of a call, for callers (the load generator)
// that need status codes and headers.
type Response struct {
	Status int
	Header http.Header
	Body   []byte
}

func (c *Client) Do(ctx context.Context, method, path string, body any, hdr map[string]string) (*Response, error) {
	var rd io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return nil, err
		}
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.BaseURL+path, rd)
	if err != nil {
		return nil, err
	}
	if c.APIKey != "" {
		req.Header.Set("Authorization", "Bearer "+c.APIKey)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	res, err := c.HTTP.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	b, err := io.ReadAll(res.Body)
	if err != nil {
		return nil, err
	}
	return &Response{Status: res.StatusCode, Header: res.Header, Body: b}, nil
}

func (c *Client) call(ctx context.Context, method, path string, body, out any, hdr map[string]string) error {
	r, err := c.Do(ctx, method, path, body, hdr)
	if err != nil {
		return err
	}
	if r.Status >= 400 {
		e := &APIError{Status: r.Status, RetryAfter: r.Header.Get("Retry-After")}
		if json.Unmarshal(r.Body, e) != nil || e.Title == "" {
			e.Title, e.Detail = http.StatusText(r.Status), strings.TrimSpace(string(r.Body))
		}
		return e
	}
	if out != nil {
		return json.Unmarshal(r.Body, out)
	}
	return nil
}

func idem(key string) map[string]string {
	if key == "" {
		return nil
	}
	return map[string]string{"Idempotency-Key": key}
}

func (c *Client) Scenarios(ctx context.Context) ([]ScenarioInfo, error) {
	var out struct {
		Data []ScenarioInfo `json:"data"`
	}
	return out.Data, c.call(ctx, "GET", "/v1/scenarios", nil, &out, nil)
}

func (c *Client) SubmitJob(ctx context.Context, req service.JobRequest, idempotencyKey string) (*Job, error) {
	var j Job
	return &j, c.call(ctx, "POST", "/v1/jobs", req, &j, idem(idempotencyKey))
}

func (c *Client) GetJob(ctx context.Context, id string) (*Job, error) {
	var j Job
	return &j, c.call(ctx, "GET", "/v1/jobs/"+url.PathEscape(id), nil, &j, nil)
}

func (c *Client) ListJobs(ctx context.Context, q url.Values) (*Page[Job], error) {
	var p Page[Job]
	return &p, c.call(ctx, "GET", "/v1/jobs?"+q.Encode(), nil, &p, nil)
}

func (c *Client) CancelJob(ctx context.Context, id string) (*Job, error) {
	var j Job
	return &j, c.call(ctx, "POST", "/v1/jobs/"+url.PathEscape(id)+"/cancel", nil, &j, nil)
}

func (c *Client) Result(ctx context.Context, id string) (*Result, error) {
	var r Result
	return &r, c.call(ctx, "GET", "/v1/jobs/"+url.PathEscape(id)+"/result", nil, &r, nil)
}

func (c *Client) History(ctx context.Context, id string) (json.RawMessage, error) {
	var r json.RawMessage
	return r, c.call(ctx, "GET", "/v1/jobs/"+url.PathEscape(id)+"/history", nil, &r, nil)
}

func (c *Client) Artifacts(ctx context.Context, id string) ([]Artifact, error) {
	var out struct {
		Data []Artifact `json:"data"`
	}
	return out.Data, c.call(ctx, "GET", "/v1/jobs/"+url.PathEscape(id)+"/artifacts", nil, &out, nil)
}

func (c *Client) Download(ctx context.Context, id, name string) ([]byte, error) {
	r, err := c.Do(ctx, "GET", "/v1/jobs/"+url.PathEscape(id)+"/artifacts/"+url.PathEscape(name), nil, nil)
	if err != nil {
		return nil, err
	}
	if r.Status != 200 {
		return nil, &APIError{Status: r.Status, Title: http.StatusText(r.Status)}
	}
	return r.Body, nil
}

func (c *Client) SubmitSweep(ctx context.Context, req service.SweepRequest, idempotencyKey string) (*Sweep, error) {
	var s Sweep
	return &s, c.call(ctx, "POST", "/v1/sweeps", req, &s, idem(idempotencyKey))
}

func (c *Client) GetSweep(ctx context.Context, id string) (*Sweep, error) {
	var s Sweep
	return &s, c.call(ctx, "GET", "/v1/sweeps/"+url.PathEscape(id), nil, &s, nil)
}

func (c *Client) ListSweeps(ctx context.Context) (*Page[Sweep], error) {
	var p Page[Sweep]
	return &p, c.call(ctx, "GET", "/v1/sweeps", nil, &p, nil)
}

func (c *Client) CancelSweep(ctx context.Context, id string) error {
	return c.call(ctx, "POST", "/v1/sweeps/"+url.PathEscape(id)+"/cancel", nil, nil, nil)
}

// SweepResults returns the raw body: JSON or CSV depending on format.
func (c *Client) SweepResults(ctx context.Context, id string, metrics []string, format string) ([]byte, error) {
	q := url.Values{}
	if len(metrics) > 0 {
		q.Set("metrics", strings.Join(metrics, ","))
	}
	if format != "" {
		q.Set("format", format)
	}
	r, err := c.Do(ctx, "GET", "/v1/sweeps/"+url.PathEscape(id)+"/results?"+q.Encode(), nil, nil)
	if err != nil {
		return nil, err
	}
	if r.Status != 200 {
		e := &APIError{Status: r.Status}
		_ = json.Unmarshal(r.Body, e)
		return nil, e
	}
	return r.Body, nil
}

// Event is one Server-Sent Event.
type Event struct {
	Name string
	Data json.RawMessage
}

// Watch streams a job's events until the server sends "done" (returned
// with a nil error), ctx ends, or the stream breaks.
func (c *Client) Watch(ctx context.Context, id string, fn func(Event)) error {
	req, err := http.NewRequestWithContext(ctx, "GET", c.BaseURL+"/v1/jobs/"+url.PathEscape(id)+"/events", nil)
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+c.APIKey)
	req.Header.Set("Accept", "text/event-stream")
	hc := *c.HTTP
	hc.Timeout = 0 // a stream lasts as long as the job
	res, err := hc.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	if res.StatusCode != 200 {
		b, _ := io.ReadAll(res.Body)
		e := &APIError{Status: res.StatusCode}
		_ = json.Unmarshal(b, e)
		return e
	}
	sc := bufio.NewScanner(res.Body)
	sc.Buffer(make([]byte, 64<<10), 4<<20)
	var ev Event
	for sc.Scan() {
		line := sc.Text()
		switch {
		case line == "":
			if ev.Name != "" {
				fn(ev)
				if ev.Name == "done" {
					return nil
				}
			}
			ev = Event{}
		case strings.HasPrefix(line, "event: "):
			ev.Name = strings.TrimPrefix(line, "event: ")
		case strings.HasPrefix(line, "data: "):
			ev.Data = json.RawMessage(strings.TrimPrefix(line, "data: "))
		}
	}
	if err := sc.Err(); err != nil {
		return err
	}
	return io.ErrUnexpectedEOF
}

// WaitFor polls until the job is terminal. Watch is better for humans;
// this is simpler for scripts and tests.
func (c *Client) WaitFor(ctx context.Context, id string, every time.Duration) (*Job, error) {
	for {
		j, err := c.GetJob(ctx, id)
		if err != nil {
			return nil, err
		}
		if j.Terminal() {
			return j, nil
		}
		select {
		case <-ctx.Done():
			return j, ctx.Err()
		case <-time.After(every):
		}
	}
}
