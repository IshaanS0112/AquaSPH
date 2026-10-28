package api

import (
	"net/http"
	"slices"
	"strconv"
	"strings"

	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/IshaanS0112/AquaSPH/backend/internal/webhooks"
)

func (s *Server) getStats(w http.ResponseWriter, r *http.Request) {
	hours := 24
	if v := r.URL.Query().Get("hours"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > 168 {
			writeProblem(w, r, Problem{Type: problemType("validation-error"), Status: http.StatusBadRequest,
				Detail: "hours must be an integer in [1, 168]"})
			return
		}
		hours = n
	}
	st, err := s.store.TenantStats(r.Context(), tenantFrom(r.Context()).ID, hours, s.liveWorkerWindow)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, st)
}

type webhookRequest struct {
	URL         string   `json:"url"`
	Description string   `json:"description"`
	EventTypes  []string `json:"event_types"`
}

func (s *Server) createWebhook(w http.ResponseWriter, r *http.Request) {
	var req webhookRequest
	if !s.decodeJSON(w, r, &req) {
		return
	}
	req.URL = strings.TrimSpace(req.URL)
	var verr service.ValidationError
	add := func(field, msg string) {
		verr.Errors = append(verr.Errors, scenario.FieldError{Field: field, Message: msg})
	}
	if err := webhooks.ValidateURL(req.URL, s.webhooksInsecure); err != nil {
		add("url", err.Error())
	}
	if len(req.Description) > 200 {
		add("description", "at most 200 characters")
	}
	if len(req.EventTypes) == 0 {
		req.EventTypes = slices.Clone(store.WebhookEventTypes)
	}
	seen := map[string]bool{}
	for _, t := range req.EventTypes {
		if !slices.Contains(store.WebhookEventTypes, t) {
			add("event_types", "unknown event type "+strconv.Quote(t)+"; use job.completed, job.failed or job.cancelled")
		}
		if seen[t] {
			add("event_types", "duplicate event type "+strconv.Quote(t))
		}
		seen[t] = true
	}
	if len(verr.Errors) > 0 {
		s.writeError(w, r, &verr)
		return
	}
	secret, err := webhooks.NewSecret()
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	ep, err := s.store.CreateWebhookEndpoint(r.Context(), tenantFrom(r.Context()).ID, req.URL, req.Description,
		req.EventTypes, secret)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	w.Header().Set("Location", "/v1/webhooks/"+ep.ID.String())
	// The only response that carries the secret: store it now.
	writeJSON(w, http.StatusCreated, ep)
}

func (s *Server) listWebhooks(w http.ResponseWriter, r *http.Request) {
	eps, err := s.store.ListWebhookEndpoints(r.Context(), tenantFrom(r.Context()).ID)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"data": eps})
}

func (s *Server) getWebhook(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	ep, err := s.store.GetWebhookEndpoint(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, ep)
}

func (s *Server) deleteWebhook(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	if err := s.store.DeleteWebhookEndpoint(r.Context(), tenantFrom(r.Context()).ID, id); err != nil {
		s.writeError(w, r, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) testWebhook(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	eventID, err := s.store.SendTestEvent(r.Context(), tenantFrom(r.Context()).ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{"event_id": eventID})
}

func (s *Server) listWebhookDeliveries(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	limit := 50
	if v := r.URL.Query().Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > maxPageSize {
			writeProblem(w, r, Problem{Type: problemType("validation-error"), Status: http.StatusBadRequest,
				Detail: "limit must be an integer in [1, 100]"})
			return
		}
		limit = n
	}
	ds, err := s.store.ListWebhookDeliveries(r.Context(), tenantFrom(r.Context()).ID, id, limit)
	if err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"endpoint_id": id, "data": ds})
}

func (s *Server) redeliverWebhook(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	deliveryID, err := strconv.ParseInt(r.PathValue("delivery"), 10, 64)
	if err != nil {
		writeProblem(w, r, Problem{Type: problemType("not-found"), Status: http.StatusNotFound})
		return
	}
	if err := s.store.RedeliverWebhook(r.Context(), tenantFrom(r.Context()).ID, id, deliveryID); err != nil {
		s.writeError(w, r, err)
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{"delivery_id": deliveryID, "state": "pending"})
}
