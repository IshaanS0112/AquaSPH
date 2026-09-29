// Package domain holds the platform's core types. It has no dependencies
// on storage or transport so every other package can share it.
package domain

import (
	"encoding/json"
	"errors"
	"time"

	"github.com/google/uuid"
)

// JobState mirrors the Postgres enum job_state. The state machine is in
// docs/platform/TRD.md §2.
type JobState string

const (
	StateQueued    JobState = "queued"
	StateRunning   JobState = "running"
	StateCompleted JobState = "completed"
	StateFailed    JobState = "failed"
	StateCancelled JobState = "cancelled"
)

// AllStates is in state-machine order; used for stable output ordering.
var AllStates = []JobState{StateQueued, StateRunning, StateCompleted, StateFailed, StateCancelled}

func (s JobState) Terminal() bool {
	return s == StateCompleted || s == StateFailed || s == StateCancelled
}

func ParseJobState(v string) (JobState, bool) {
	for _, s := range AllStates {
		if string(s) == v {
			return s, true
		}
	}
	return "", false
}

// Outcome of a completed job. UNSTABLE is a result, not a failure: the
// physics diverged, deterministically, and re-running would diverge again.
const (
	OutcomeStable   = "stable"
	OutcomeUnstable = "unstable"
)

// Error codes recorded on failed (or retried) jobs. They are part of the
// API contract: clients branch on them.
const (
	ErrCodeInvalidScenario = "invalid_scenario" // solver exit 2; not retried
	ErrCodeTimeout         = "timeout"          // wall-clock limit; not retried
	ErrCodeSolverCrashed   = "solver_crashed"   // killed by a signal we did not send; retried
	ErrCodeLeaseExpired    = "lease_expired"    // worker vanished; retried by the reaper
	ErrCodeInternal        = "internal"         // worker-side infrastructure failure; retried
)

var (
	// ErrNotFound covers both "does not exist" and "belongs to another tenant".
	ErrNotFound = errors.New("not found")
	// ErrLeaseLost means a worker's fenced write matched no row: its lease
	// expired and the job was reclaimed. The worker must discard its work.
	ErrLeaseLost = errors.New("lease lost")
	// ErrConflict is an operation that is invalid in the resource's
	// current state, e.g. cancelling a completed job.
	ErrConflict = errors.New("conflict")
)

// QuotaError is returned when a submission would exceed a tenant limit.
type QuotaError struct {
	Limit   string
	Max     int
	Current int
}

func (e *QuotaError) Error() string {
	return "quota exceeded: " + e.Limit
}

// IdempotencyMismatchError: the Idempotency-Key was already used with a different request body.
type IdempotencyMismatchError struct{ Key string }

func (e *IdempotencyMismatchError) Error() string {
	return "idempotency key reused with a different request: " + e.Key
}

type Tenant struct {
	ID                uuid.UUID `json:"id"`
	Name              string    `json:"name"`
	MaxConcurrentJobs int       `json:"max_concurrent_jobs"`
	MaxQueuedJobs     int       `json:"max_queued_jobs"`
	MaxParticles      int       `json:"max_particles"`
	MaxWallSeconds    int       `json:"max_wall_seconds"`
	RatePerMinute     int       `json:"rate_per_minute"`
	RateBurst         int       `json:"rate_burst"`
	CreatedAt         time.Time `json:"created_at"`
}

type APIKey struct {
	ID         uuid.UUID  `json:"id"`
	TenantID   uuid.UUID  `json:"tenant_id"`
	Prefix     string     `json:"prefix"`
	SecretHash []byte     `json:"-"`
	Name       string     `json:"name"`
	CreatedAt  time.Time  `json:"created_at"`
	LastUsedAt *time.Time `json:"last_used_at,omitempty"`
	RevokedAt  *time.Time `json:"revoked_at,omitempty"`
}

// Progress is the latest solver progress sample, stored on the job row by
// the worker heartbeat and fanned out over SSE.
type Progress struct {
	T        float64  `json:"t"`
	TEnd     float64  `json:"t_end"`
	Fraction float64  `json:"fraction"`
	Step     int64    `json:"step"`
	Fluid    int64    `json:"fluid"`
	MaxSpeed *float64 `json:"max_speed,omitempty"`
	WallS    float64  `json:"wall_s"`
}

// Artifact describes one stored output file of a job.
type Artifact struct {
	Name        string `json:"name"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	ContentType string `json:"content_type"`
}

type Job struct {
	ID          uuid.UUID
	TenantID    uuid.UUID
	SweepID     *uuid.UUID
	SweepParams json.RawMessage
	State       JobState
	Outcome     *string
	Priority    int

	Scenario       string
	Quality        string
	SimTime        *float64
	MaxSteps       *int
	MaxParticles   int
	TimeoutSeconds int
	Spec           json.RawMessage
	Overrides      json.RawMessage
	Labels         json.RawMessage
	SpecHash       string
	AllowCache     bool

	Attempt           int
	MaxAttempts       int
	RunAfter          time.Time
	LeaseToken        *uuid.UUID
	LeaseExpiresAt    *time.Time
	WorkerID          *uuid.UUID
	CancelRequestedAt *time.Time
	Progress          json.RawMessage

	SolverID      *string
	CacheKey      *string
	CacheHit      bool
	SourceJobID   *uuid.UUID
	ArtifactJobID *uuid.UUID
	Artifacts     json.RawMessage
	Result        json.RawMessage
	ErrorCode     *string
	ErrorMessage  *string

	CreatedAt  time.Time
	StartedAt  *time.Time
	FinishedAt *time.Time
	UpdatedAt  time.Time
}

type Sweep struct {
	ID            uuid.UUID       `json:"id"`
	TenantID      uuid.UUID       `json:"-"`
	Scenario      string          `json:"scenario"`
	Quality       string          `json:"quality"`
	Grid          json.RawMessage `json:"grid"`
	BaseOverrides json.RawMessage `json:"overrides"`
	Total         int             `json:"total"`
	Labels        json.RawMessage `json:"labels"`
	CreatedAt     time.Time       `json:"created_at"`
}

// JobEvent is one row of the audit trail written by the jobs_audit trigger.
type JobEvent struct {
	At        time.Time  `json:"at"`
	FromState *JobState  `json:"from_state"`
	ToState   JobState   `json:"to_state"`
	Attempt   int        `json:"attempt"`
	WorkerID  *uuid.UUID `json:"worker_id,omitempty"`
	Detail    *string    `json:"detail,omitempty"`
}

type Worker struct {
	ID          uuid.UUID  `json:"id"`
	Hostname    string     `json:"hostname"`
	PID         int        `json:"pid"`
	SolverID    string     `json:"solver_id"`
	Version     string     `json:"version"`
	Concurrency int        `json:"concurrency"`
	StartedAt   time.Time  `json:"started_at"`
	HeartbeatAt time.Time  `json:"heartbeat_at"`
	StoppedAt   *time.Time `json:"stopped_at,omitempty"`
}
