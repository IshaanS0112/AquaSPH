package api

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
)

// Problem is an RFC 9457 problem details body. Every error the API
// returns has this shape, so a client needs exactly one error parser.
type Problem struct {
	Type      string                `json:"type"`
	Title     string                `json:"title"`
	Status    int                   `json:"status"`
	Detail    string                `json:"detail,omitempty"`
	Instance  string                `json:"instance,omitempty"`
	RequestID string                `json:"request_id,omitempty"`
	Errors    []scenario.FieldError `json:"errors,omitempty"`
	Limit     string                `json:"limit,omitempty"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}

func writeProblem(w http.ResponseWriter, r *http.Request, p Problem) {
	if p.Type == "" {
		p.Type = "about:blank"
	}
	if p.Title == "" {
		p.Title = http.StatusText(p.Status)
	}
	p.Instance = r.URL.Path
	p.RequestID = requestIDFrom(r.Context())
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(p.Status)
	_ = json.NewEncoder(w).Encode(p)
}

func problemType(slug string) string { return "/problems/" + slug }

// writeError maps a domain or service error to a problem response.
func (s *Server) writeError(w http.ResponseWriter, r *http.Request, err error) {
	var v *service.ValidationError
	var q *domain.QuotaError
	var m *domain.IdempotencyMismatchError
	switch {
	case errors.As(err, &v):
		writeProblem(w, r, Problem{Type: problemType("validation-error"), Title: "Request validation failed",
			Status: http.StatusUnprocessableEntity, Detail: v.Error(), Errors: v.Errors})
	case errors.As(err, &q):
		w.Header().Set("Retry-After", "30")
		writeProblem(w, r, Problem{Type: problemType("quota-exceeded"), Title: "Tenant quota exceeded",
			Status: http.StatusTooManyRequests, Limit: q.Limit,
			Detail: fmt.Sprintf("%s is %d and %d are in use; wait for jobs to finish", q.Limit, q.Max, q.Current)})
	case errors.As(err, &m):
		writeProblem(w, r, Problem{Type: problemType("idempotency-key-reused"), Title: "Idempotency key reused",
			Status: http.StatusUnprocessableEntity,
			Detail: "this Idempotency-Key was already used with a different request body"})
	case errors.Is(err, domain.ErrNotFound):
		writeProblem(w, r, Problem{Type: problemType("not-found"), Status: http.StatusNotFound})
	case errors.Is(err, domain.ErrConflict):
		writeProblem(w, r, Problem{Type: problemType("conflict"), Status: http.StatusConflict,
			Detail: "the resource is not in a state that allows this operation"})
	default:
		s.log.ErrorContext(r.Context(), "internal error", "err", err, "request_id", requestIDFrom(r.Context()))
		writeProblem(w, r, Problem{Status: http.StatusInternalServerError,
			Detail: "internal error; quote the request_id when reporting it"})
	}
}

// decodeJSON reads a JSON body strictly: size-limited, unknown fields rejected (so "sim_tme" is
// an error, not a silently ignored typo), and exactly one JSON value.
func (s *Server) decodeJSON(w http.ResponseWriter, r *http.Request, dst any) bool {
	if ct := r.Header.Get("Content-Type"); ct != "" && !strings.HasPrefix(ct, "application/json") {
		writeProblem(w, r, Problem{Type: problemType("unsupported-media-type"), Status: http.StatusUnsupportedMediaType,
			Detail: "send application/json"})
		return false
	}
	body := http.MaxBytesReader(w, r.Body, s.maxBodyBytes)
	dec := json.NewDecoder(body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		var tooBig *http.MaxBytesError
		if errors.As(err, &tooBig) {
			writeProblem(w, r, Problem{Type: problemType("body-too-large"), Status: http.StatusRequestEntityTooLarge,
				Detail: fmt.Sprintf("request body exceeds %d bytes", s.maxBodyBytes)})
			return false
		}
		writeProblem(w, r, Problem{Type: problemType("malformed-json"), Status: http.StatusBadRequest,
			Detail: err.Error()})
		return false
	}
	if _, err := dec.Token(); err != io.EOF {
		writeProblem(w, r, Problem{Type: problemType("malformed-json"), Status: http.StatusBadRequest,
			Detail: "request body must contain a single JSON value"})
		return false
	}
	return true
}
