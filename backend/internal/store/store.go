// Package store is the Postgres data-access layer for everything except
// the queue's claim/lease protocol (package queue). Every tenant-facing
// read and write takes the tenant ID and filters on it in SQL: isolation
// is enforced by the query, not by a check in the handler that a future
// handler could forget.
package store

import (
	"context"
	"errors"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// NotifyChannel is the LISTEN/NOTIFY channel that wakes idle workers.
const NotifyChannel = "aquasph_jobs"

type Store struct {
	pool *pgxpool.Pool
}

func New(pool *pgxpool.Pool) *Store { return &Store{pool: pool} }

func (s *Store) Pool() *pgxpool.Pool { return s.pool }

// Querier is satisfied by both the pool and a transaction.
type Querier interface {
	Exec(ctx context.Context, sql string, args ...any) (pgconn.CommandTag, error)
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
}

func notFound(err error) error {
	if errors.Is(err, pgx.ErrNoRows) {
		return domain.ErrNotFound
	}
	return err
}

// IsUniqueViolation reports whether err is a unique-constraint violation.
func IsUniqueViolation(err error) bool {
	var pg *pgconn.PgError
	return errors.As(err, &pg) && pg.Code == "23505"
}

// NotifyWorkers wakes idle workers once tx commits. NOTIFY inside a
// transaction is delivered on commit and dropped on rollback, which is
// exactly the semantics enqueue needs.
func NotifyWorkers(ctx context.Context, q Querier) error {
	_, err := q.Exec(ctx, `SELECT pg_notify($1, '')`, NotifyChannel)
	return err
}
