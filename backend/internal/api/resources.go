package api

import (
	"encoding/json"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/google/uuid"
)

type jobError struct {
	Code    string `json:"code"`
	Message string `json:"message,omitempty"`
}

type jobLinks struct {
	Self      string `json:"self"`
	Events    string `json:"events"`
	Result    string `json:"result"`
	Artifacts string `json:"artifacts"`
	History   string `json:"history"`
	Sweep     string `json:"sweep,omitempty"`
}

// jobResource is the public representation of a job.
type jobResource struct {
	ID              uuid.UUID       `json:"id"`
	State           domain.JobState `json:"state"`
	Outcome         *string         `json:"outcome"`
	Scenario        string          `json:"scenario"`
	Quality         string          `json:"quality"`
	SimTime         *float64        `json:"sim_time,omitempty"`
	MaxSteps        *int            `json:"max_steps,omitempty"`
	MaxParticles    int             `json:"max_particles"`
	TimeoutSeconds  int             `json:"timeout_seconds"`
	Priority        int             `json:"priority"`
	Overrides       json.RawMessage `json:"overrides"`
	Labels          json.RawMessage `json:"labels"`
	SweepID         *uuid.UUID      `json:"sweep_id,omitempty"`
	SweepParams     json.RawMessage `json:"sweep_params,omitempty"`
	Progress        json.RawMessage `json:"progress"`
	Attempt         int             `json:"attempt"`
	MaxAttempts     int             `json:"max_attempts"`
	CancelRequested bool            `json:"cancel_requested"`
	CacheHit        bool            `json:"cache_hit"`
	SourceJobID     *uuid.UUID      `json:"source_job_id,omitempty"`
	SpecHash        string          `json:"spec_hash"`
	SolverID        *string         `json:"solver_id"`
	Error           *jobError       `json:"error"`
	Spec            json.RawMessage `json:"spec,omitempty"`
	CreatedAt       time.Time       `json:"created_at"`
	StartedAt       *time.Time      `json:"started_at"`
	FinishedAt      *time.Time      `json:"finished_at"`
	Links           jobLinks        `json:"links"`
}

func nullIfEmpty(b json.RawMessage) json.RawMessage {
	if len(b) == 0 {
		return json.RawMessage("null")
	}
	return b
}

func toJobResource(j *domain.Job, includeSpec bool) jobResource {
	self := "/v1/jobs/" + j.ID.String()
	r := jobResource{
		ID: j.ID, State: j.State, Outcome: j.Outcome, Scenario: j.Scenario, Quality: j.Quality,
		SimTime: j.SimTime, MaxSteps: j.MaxSteps, MaxParticles: j.MaxParticles, TimeoutSeconds: j.TimeoutSeconds,
		Priority: j.Priority, Overrides: j.Overrides, Labels: j.Labels, SweepID: j.SweepID,
		Progress: nullIfEmpty(j.Progress), Attempt: j.Attempt, MaxAttempts: j.MaxAttempts,
		CancelRequested: j.CancelRequestedAt != nil, CacheHit: j.CacheHit, SourceJobID: j.SourceJobID,
		SpecHash: j.SpecHash, SolverID: j.SolverID, CreatedAt: j.CreatedAt, StartedAt: j.StartedAt,
		FinishedAt: j.FinishedAt,
		Links: jobLinks{Self: self, Events: self + "/events", Result: self + "/result",
			Artifacts: self + "/artifacts", History: self + "/history"},
	}
	if len(j.SweepParams) > 0 {
		r.SweepParams = j.SweepParams
	}
	if j.SweepID != nil {
		r.Links.Sweep = "/v1/sweeps/" + j.SweepID.String()
	}
	// A retried job keeps its last error as history while queued; only a
	// failed job reports it as *the* error.
	if j.ErrorCode != nil && (j.State == domain.StateFailed || j.State == domain.StateQueued) {
		e := &jobError{Code: *j.ErrorCode}
		if j.ErrorMessage != nil {
			e.Message = *j.ErrorMessage
		}
		r.Error = e
	}
	if includeSpec {
		r.Spec = j.Spec
	}
	return r
}

type sweepResource struct {
	ID        uuid.UUID               `json:"id"`
	State     string                  `json:"state"`
	Scenario  string                  `json:"scenario"`
	Quality   string                  `json:"quality"`
	Grid      json.RawMessage         `json:"grid"`
	Overrides json.RawMessage         `json:"overrides"`
	Labels    json.RawMessage         `json:"labels"`
	Total     int                     `json:"total"`
	Counts    map[domain.JobState]int `json:"counts"`
	CacheHits int                     `json:"cache_hits"`
	CreatedAt time.Time               `json:"created_at"`
	Links     map[string]string       `json:"links"`
}

func toSweepResource(sw *domain.Sweep, counts map[domain.JobState]int, hits int) sweepResource {
	state := "completed"
	if counts[domain.StateQueued]+counts[domain.StateRunning] > 0 {
		state = "running"
	}
	self := "/v1/sweeps/" + sw.ID.String()
	return sweepResource{
		ID: sw.ID, State: state, Scenario: sw.Scenario, Quality: sw.Quality, Grid: sw.Grid,
		Overrides: sw.BaseOverrides, Labels: sw.Labels, Total: sw.Total, Counts: counts, CacheHits: hits,
		CreatedAt: sw.CreatedAt,
		Links: map[string]string{"self": self, "results": self + "/results",
			"jobs": "/v1/jobs?sweep_id=" + sw.ID.String()},
	}
}

type page[T any] struct {
	Data       []T     `json:"data"`
	NextCursor *string `json:"next_cursor"`
}

func newPage[T any](data []T, next string) page[T] {
	if data == nil {
		data = []T{}
	}
	p := page[T]{Data: data}
	if next != "" {
		p.NextCursor = &next
	}
	return p
}
