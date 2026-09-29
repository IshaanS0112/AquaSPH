package api

import (
	"encoding/csv"
	"fmt"
	"net/http"
	"strconv"
	"strings"

	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
)

func (s *Server) createSweep(w http.ResponseWriter, r *http.Request) {
	var req service.SweepRequest
	if !s.decodeJSON(w, r, &req) {
		return
	}
	key, ok := idempotencyKey(w, r)
	if !ok {
		return
	}
	tenant := tenantFrom(r.Context())
	sw, replayed, err := s.svc.SubmitSweep(r.Context(), tenant, req, key)
	if err != nil {
		s.metrics.Submissions.WithLabelValues("sweep", "rejected").Inc()
		s.writeError(w, r, err)
		return
	}
	result := "queued"
	if replayed {
		result = "replayed"
		w.Header().Set("Idempotent-Replayed", "true")
	}
	s.metrics.Submissions.WithLabelValues("sweep", result).Inc()
	counts, hits, err := s.store.SweepCounts(r.Context(), sw.ID)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	w.Header().Set("Location", "/v1/sweeps/"+sw.ID.String())
	writeJSON(w, http.StatusCreated, toSweepResource(sw, counts, hits))
}

func (s *Server) getSweep(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	sw, err := s.store.GetSweep(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	counts, hits, err := s.store.SweepCounts(r.Context(), sw.ID)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, toSweepResource(sw, counts, hits))
}

func (s *Server) listSweeps(w http.ResponseWriter, r *http.Request) {
	limit, after, ok := pageParams(w, r)
	if !ok {
		return
	}
	sweeps, next, err := s.store.ListSweeps(r.Context(), tenantFrom(r.Context()).ID, after, limit)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	out := make([]sweepResource, len(sweeps))
	for i, sw := range sweeps {
		counts, hits, err := s.store.SweepCounts(r.Context(), sw.ID)
		if err != nil {
			s.writeError(w, r, err)
			return
		}
		out[i] = toSweepResource(sw, counts, hits)
	}
	writeJSON(w, http.StatusOK, newPage(out, next))
}

func (s *Server) cancelSweep(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	n, err := s.store.CancelSweep(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{"sweep_id": id, "jobs_cancelled_or_flagged": n})
}

func (s *Server) sweepResults(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	var metrics []string
	if m := r.URL.Query().Get("metrics"); m != "" {
		metrics = strings.Split(m, ",")
	}
	res, err := s.svc.Results(r.Context(), tenantFrom(r.Context()).ID, id, metrics)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	switch r.URL.Query().Get("format") {
	case "", "json":
		writeJSON(w, http.StatusOK, res)
	case "csv":
		w.Header().Set("Content-Type", "text/csv; charset=utf-8")
		w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="sweep-%s.csv"`, id))
		cw := csv.NewWriter(w)
		header := append([]string{"job_id", "state", "outcome", "cache_hit"}, res.Axes...)
		cw.Write(append(header, res.Metrics...))
		for _, row := range res.Rows {
			outcome := ""
			if row.Outcome != nil {
				outcome = *row.Outcome
			}
			rec := []string{row.JobID.String(), row.State, outcome, strconv.FormatBool(row.CacheHit)}
			for _, a := range res.Axes {
				rec = append(rec, csvCell(row.Params[a]))
			}
			for _, m := range res.Metrics {
				rec = append(rec, csvCell(row.Metrics[m]))
			}
			cw.Write(rec)
		}
		cw.Flush()
	default:
		writeProblem(w, r, Problem{Type: problemType("validation-error"), Status: http.StatusBadRequest,
			Detail: "format must be json or csv"})
	}
}

// csvCell renders one value. Strings that a spreadsheet would evaluate as a formula (=, +, -,
// @, tab, CR at the start) are prefixed with a quote: grid values are user-supplied, and a
// results CSV is exactly the file someone opens in Excel.
func csvCell(v any) string {
	switch t := v.(type) {
	case nil:
		return ""
	case float64:
		return strconv.FormatFloat(t, 'g', -1, 64)
	case bool:
		return strconv.FormatBool(t)
	case string:
		if t != "" && strings.ContainsRune("=+-@\t\r", rune(t[0])) {
			return "'" + t
		}
		return t
	default:
		return fmt.Sprint(t)
	}
}
