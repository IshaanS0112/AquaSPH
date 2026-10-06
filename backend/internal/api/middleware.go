package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"log/slog"
	"net/http"
	"regexp"
	"runtime/debug"
	"strconv"
	"strings"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/auth"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
)

type ctxKey int

const (
	keyRequestID ctxKey = iota
	keyTenant
	keyReqInfo
)

// reqInfo is filled in by inner middleware (auth) and read by the outer
// logging middleware after the handler returns.
type reqInfo struct {
	tenant string
}

func requestIDFrom(ctx context.Context) string {
	id, _ := ctx.Value(keyRequestID).(string)
	return id
}

func tenantFrom(ctx context.Context) *domain.Tenant {
	t, _ := ctx.Value(keyTenant).(*domain.Tenant)
	return t
}

var requestIDRE = regexp.MustCompile(`^[A-Za-z0-9._-]{1,64}$`)

func newRequestID() string {
	b := make([]byte, 12)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

type statusWriter struct {
	http.ResponseWriter
	status int
	bytes  int
}

func (w *statusWriter) WriteHeader(code int) {
	if w.status == 0 {
		w.status = code
	}
	w.ResponseWriter.WriteHeader(code)
}

func (w *statusWriter) Write(b []byte) (int, error) {
	if w.status == 0 {
		w.status = http.StatusOK
	}
	n, err := w.ResponseWriter.Write(b)
	w.bytes += n
	return n, err
}

// Unwrap lets http.ResponseController reach Flush and SetWriteDeadline
// on the real writer (SSE needs both).
func (w *statusWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }

// observe is the outermost middleware: request ID, panic recovery,
// security headers, access log and metrics.
func (s *Server) observe(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rid := r.Header.Get("X-Request-ID")
		if !requestIDRE.MatchString(rid) {
			rid = newRequestID()
		}
		info := &reqInfo{}
		ctx := context.WithValue(r.Context(), keyRequestID, rid)
		ctx = context.WithValue(ctx, keyReqInfo, info)
		r = r.WithContext(ctx)

		h := w.Header()
		h.Set("X-Request-ID", rid)
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("Cache-Control", "no-store") // responses carry tenant data
		sw := &statusWriter{ResponseWriter: w}

		defer func() {
			if rec := recover(); rec != nil {
				s.log.Error("panic", "panic", rec, "request_id", rid, "stack", string(debug.Stack()))
				if sw.status == 0 {
					writeProblem(sw, r, Problem{Status: http.StatusInternalServerError, Detail: "internal error"})
				}
			}
			route := r.Pattern
			if route == "" {
				route = "unmatched"
			}
			if sw.status == 0 {
				sw.status = http.StatusOK
			}
			elapsed := time.Since(start)
			s.metrics.Requests.WithLabelValues(route, strconv.Itoa(sw.status)).Inc()
			if !strings.HasSuffix(route, "/events") { // SSE streams are long-lived by design
				s.metrics.RequestDuration.WithLabelValues(route).Observe(elapsed.Seconds())
			}
			level := slog.LevelInfo
			if sw.status >= 500 {
				level = slog.LevelError
			}
			s.log.Log(r.Context(), level, "request", "method", r.Method, "route", route, "path", r.URL.Path,
				"status", sw.status, "bytes", sw.bytes, "duration_ms", float64(elapsed.Microseconds())/1000,
				"request_id", rid, "tenant", info.tenant)
		}()
		next.ServeHTTP(sw, r)
	})
}

// authenticate resolves the bearer API key to a tenant (ADR-0004), then
// applies the tenant's rate limit.
func (s *Server) authenticate(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		unauthorized := func(detail string) {
			w.Header().Set("WWW-Authenticate", `Bearer realm="aquasph"`)
			writeProblem(w, r, Problem{Type: problemType("unauthorized"), Status: http.StatusUnauthorized, Detail: detail})
		}
		hdr := r.Header.Get("Authorization")
		key, ok := strings.CutPrefix(hdr, "Bearer ")
		if !ok {
			unauthorized("send 'Authorization: Bearer <api key>'")
			return
		}
		prefix, err := auth.Parse(key)
		if err != nil {
			unauthorized("malformed API key")
			return
		}
		k, tenant, err := s.store.LookupAPIKey(r.Context(), prefix)
		// Same response for "no such key" and "wrong secret", and the hash comparison is
		// constant-time, so a prefix learned from a log line gives an attacker nothing to iterate on.
		if err != nil || !auth.Verify(key, k.SecretHash) {
			if err != nil && !errors.Is(err, domain.ErrNotFound) {
				s.writeError(w, r, err)
				return
			}
			unauthorized("invalid API key")
			return
		}
		if k.RevokedAt != nil {
			unauthorized("API key revoked")
			return
		}
		s.touchKey(k)
		if info, ok := r.Context().Value(keyReqInfo).(*reqInfo); ok {
			info.tenant = tenant.Name
		}

		if s.limiter != nil {
			d, err := s.limiter.Allow(r.Context(), tenant.ID.String(), tenant.RatePerMinute, tenant.RateBurst)
			if err != nil {
				// Fail open: Redis being down should degrade protection, not take the API down with it.
				s.metrics.RateLimitErrors.Inc()
				s.log.Warn("rate limiter unavailable; allowing request", "err", err)
			} else {
				w.Header().Set("RateLimit-Limit", strconv.Itoa(d.Limit))
				w.Header().Set("RateLimit-Remaining", strconv.Itoa(d.Remaining))
				w.Header().Set("RateLimit-Reset", strconv.Itoa(int(d.ResetAfter.Seconds()+0.999)))
				if !d.Allowed {
					s.metrics.RateLimited.Inc()
					w.Header().Set("Retry-After", strconv.Itoa(int(d.RetryAfter.Seconds()+0.999)))
					writeProblem(w, r, Problem{Type: problemType("rate-limited"), Status: http.StatusTooManyRequests,
						Detail: "request rate limit exceeded; see Retry-After"})
					return
				}
			}
		}
		next(w, r.WithContext(context.WithValue(r.Context(), keyTenant, tenant)))
	}
}

// touchKey updates last_used_at in the background, at most once a minute
// per key per process, so authentication stays a read.
func (s *Server) touchKey(k *domain.APIKey) {
	now := time.Now()
	if v, ok := s.touched.Load(k.ID); ok && now.Sub(v.(time.Time)) < time.Minute {
		return
	}
	s.touched.Store(k.ID, now)
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = s.store.TouchAPIKey(ctx, k.ID)
	}()
}
