package api

import (
	"encoding/json"
	"net/http"
	"strconv"
	"strings"

	"github.com/IshaanS0112/AquaSPH/backend/internal/artifacts"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/google/uuid"
)

// pathID parses an {id} path value. A malformed ID is a 404, not a 400:
// "no such job" is the truthful answer, and it gives nothing away.
func (s *Server) pathID(w http.ResponseWriter, r *http.Request) (uuid.UUID, bool) {
	id, err := uuid.Parse(r.PathValue("id"))
	if err != nil {
		writeProblem(w, r, Problem{Type: problemType("not-found"), Status: http.StatusNotFound})
		return uuid.Nil, false
	}
	return id, true
}

func idempotencyKey(w http.ResponseWriter, r *http.Request) (string, bool) {
	k := r.Header.Get("Idempotency-Key")
	if len(k) > 255 {
		writeProblem(w, r, Problem{Type: problemType("validation-error"), Status: http.StatusBadRequest,
			Detail: "Idempotency-Key longer than 255 characters"})
		return "", false
	}
	return k, true
}

func (s *Server) listScenarios(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"data": s.catalog.List()})
}

func (s *Server) getScenario(w http.ResponseWriter, r *http.Request) {
	e, ok := s.catalog.Get(r.PathValue("name"))
	if !ok {
		writeProblem(w, r, Problem{Type: problemType("not-found"), Status: http.StatusNotFound})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"name": e.Name, "tier": e.Tier, "description": e.Description,
		"approximation": e.Approximation, "spec": e.Spec})
}

func (s *Server) createJob(w http.ResponseWriter, r *http.Request) {
	var req service.JobRequest
	if !s.decodeJSON(w, r, &req) {
		return
	}
	key, ok := idempotencyKey(w, r)
	if !ok {
		return
	}
	job, replayed, err := s.svc.SubmitJob(r.Context(), tenantFrom(r.Context()), req, key)
	if err != nil {
		s.metrics.Submissions.WithLabelValues("job", "rejected").Inc()
		s.writeError(w, r, err)
		return
	}
	switch {
	case replayed:
		s.metrics.Submissions.WithLabelValues("job", "replayed").Inc()
		w.Header().Set("Idempotent-Replayed", "true")
	case job.CacheHit:
		s.metrics.Submissions.WithLabelValues("job", "cache_hit").Inc()
	default:
		s.metrics.Submissions.WithLabelValues("job", "queued").Inc()
	}
	w.Header().Set("Location", "/v1/jobs/"+job.ID.String())
	writeJSON(w, http.StatusCreated, toJobResource(job, false))
}

func (s *Server) getJob(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	job, err := s.store.GetJob(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, toJobResource(job, r.URL.Query().Get("include") == "spec"))
}

const maxPageSize = 100

func pageParams(w http.ResponseWriter, r *http.Request) (limit int, after *uuid.UUID, ok bool) {
	q := r.URL.Query()
	limit = 20
	if v := q.Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > maxPageSize {
			writeProblem(w, r, Problem{Type: problemType("validation-error"), Status: http.StatusBadRequest,
				Detail: "limit must be an integer in [1, 100]"})
			return 0, nil, false
		}
		limit = n
	}
	if c := q.Get("cursor"); c != "" {
		id, err := store.DecodeCursor(c)
		if err != nil {
			writeProblem(w, r, Problem{Type: problemType("validation-error"), Status: http.StatusBadRequest,
				Detail: "invalid cursor"})
			return 0, nil, false
		}
		after = &id
	}
	return limit, after, true
}

func (s *Server) listJobs(w http.ResponseWriter, r *http.Request) {
	limit, after, ok := pageParams(w, r)
	if !ok {
		return
	}
	q := r.URL.Query()
	var f store.JobFilter
	bad := func(detail string) {
		writeProblem(w, r, Problem{Type: problemType("validation-error"), Status: http.StatusBadRequest, Detail: detail})
	}
	if v := q.Get("state"); v != "" {
		st, ok := domain.ParseJobState(v)
		if !ok {
			bad("state must be one of queued, running, completed, failed, cancelled")
			return
		}
		f.State = &st
	}
	f.Scenario = q.Get("scenario")
	if v := q.Get("sweep_id"); v != "" {
		id, err := uuid.Parse(v)
		if err != nil {
			bad("sweep_id is not a UUID")
			return
		}
		f.SweepID = &id
	}
	for _, l := range q["label"] {
		k, v, found := strings.Cut(l, ":")
		if !found || k == "" {
			bad("label filters look like label=key:value")
			return
		}
		if f.Labels == nil {
			f.Labels = map[string]string{}
		}
		f.Labels[k] = v
	}
	jobs, next, err := s.store.ListJobs(r.Context(), tenantFrom(r.Context()).ID, f, after, limit)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	out := make([]jobResource, len(jobs))
	for i, j := range jobs {
		out[i] = toJobResource(j, false)
	}
	writeJSON(w, http.StatusOK, newPage(out, next))
}

func (s *Server) cancelJob(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	job, err := s.store.CancelJob(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	// 202: a running job is only flagged here; its worker stops the solver
	// at its next heartbeat and the state becomes cancelled then.
	writeJSON(w, http.StatusAccepted, toJobResource(job, false))
}

func (s *Server) jobHistory(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	evs, err := s.store.JobHistory(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"job_id": id, "events": evs})
}

func (s *Server) jobResult(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	job, err := s.store.GetJob(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	if len(job.Result) == 0 {
		writeProblem(w, r, Problem{Type: problemType("result-not-available"), Status: http.StatusConflict,
			Detail: "no result: the job is " + string(job.State)})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"job_id": job.ID, "state": job.State, "outcome": job.Outcome, "cache_hit": job.CacheHit,
		// A cancelled or timed-out run still has metrics, describing the
		// run up to where it stopped.
		"partial": job.State != domain.StateCompleted,
		"metrics": json.RawMessage(job.Result),
	})
}

func jobArtifacts(job *domain.Job) []domain.Artifact {
	var list []domain.Artifact
	_ = json.Unmarshal(job.Artifacts, &list)
	return list
}

func (s *Server) listArtifacts(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	job, err := s.store.GetJob(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	type item struct {
		domain.Artifact
		URL string `json:"url"`
	}
	list := jobArtifacts(job)
	out := make([]item, len(list))
	for i, a := range list {
		out[i] = item{Artifact: a, URL: "/v1/jobs/" + id.String() + "/artifacts/" + a.Name}
	}
	writeJSON(w, http.StatusOK, map[string]any{"job_id": id, "data": out})
}

// getArtifact streams a file. The name must appear in the job's recorded artifact list (an
// allowlist from the database), and the artifact store validates it again, so the URL cannot
// address anything else.
func (s *Server) getArtifact(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	job, err := s.store.GetJob(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	name := r.PathValue("name")
	var meta *domain.Artifact
	for _, a := range jobArtifacts(job) {
		if a.Name == name {
			meta = &a
			break
		}
	}
	if meta == nil || job.ArtifactJobID == nil {
		writeProblem(w, r, Problem{Type: problemType("not-found"), Status: http.StatusNotFound})
		return
	}
	f, err := s.artifacts.Open(r.Context(), *job.ArtifactJobID, name)
	if err == artifacts.ErrNotFound {
		writeProblem(w, r, Problem{Type: problemType("not-found"), Status: http.StatusNotFound,
			Detail: "artifact recorded but missing from storage"})
		return
	}
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	w.Header().Set("Content-Type", meta.ContentType)
	w.Header().Set("ETag", `"`+meta.SHA256+`"`) // content hash: a strong validator
	w.Header().Set("Cache-Control", "private, max-age=31536000, immutable")
	http.ServeContent(w, r, name, st.ModTime(), f) // handles Range and If-None-Match
}
