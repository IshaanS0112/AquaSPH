// Package testutil provides real Postgres and Redis instances to integration tests.
package testutil

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"log/slog"
	"net/url"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/db"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

func env(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

// unavailable skips the test locally, but fails it in CI, where a missing
// database means the integration suite silently stopped running.
func unavailable(t testing.TB, what string, err error) {
	t.Helper()
	if os.Getenv("AQUASPH_REQUIRE_INTEGRATION") == "1" {
		t.Fatalf("%s unavailable and AQUASPH_REQUIRE_INTEGRATION=1: %v", what, err)
	}
	t.Skipf("%s unavailable (%v); set AQUASPH_TEST_DATABASE_URL / AQUASPH_TEST_REDIS_URL", what, err)
}

func randomSuffix() string {
	b := make([]byte, 6)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

// NewDB creates an isolated, migrated database and returns a pool plus its
// URL (for code under test that opens its own connections, like LISTEN).
func NewDB(t testing.TB) (*pgxpool.Pool, string) {
	t.Helper()
	adminURL := env("AQUASPH_TEST_DATABASE_URL", "postgres://postgres@127.0.0.1:5432/postgres?sslmode=disable")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	admin, err := pgx.Connect(ctx, adminURL)
	if err != nil {
		unavailable(t, "postgres", err)
	}
	defer admin.Close(context.Background())

	name := "aqtest_" + randomSuffix()
	if _, err := admin.Exec(ctx, "CREATE DATABASE "+name); err != nil {
		t.Fatalf("create database: %v", err)
	}
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		c, err := pgx.Connect(ctx, adminURL)
		if err != nil {
			return
		}
		defer c.Close(ctx)
		_, _ = c.Exec(ctx, "DROP DATABASE IF EXISTS "+name+" WITH (FORCE)")
	})

	u, err := url.Parse(adminURL)
	if err != nil {
		t.Fatalf("parse admin url: %v", err)
	}
	u.Path = "/" + name
	dbURL := u.String()

	pool, err := db.Connect(ctx, dbURL, 20)
	if err != nil {
		t.Fatalf("connect test db: %v", err)
	}
	t.Cleanup(pool.Close)
	if _, err := db.Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	return pool, dbURL
}

// RedisURL returns the test Redis URL after checking it is reachable.
func RedisURL(t testing.TB) string {
	t.Helper()
	u := env("AQUASPH_TEST_REDIS_URL", "redis://127.0.0.1:6379/0")
	if err := pingRedis(u); err != nil {
		unavailable(t, "redis", err)
	}
	return u
}

// Unique returns a name that will not collide across parallel tests.
func Unique(prefix string) string {
	return fmt.Sprintf("%s-%s", prefix, randomSuffix())
}

// Logger returns a slog.Logger that writes through t.Log, so output
// appears only for failing tests.
func Logger(t testing.TB) *slog.Logger {
	return slog.New(slog.NewTextHandler(testWriter{t}, &slog.HandlerOptions{Level: slog.LevelDebug}))
}

type testWriter struct{ t testing.TB }

func (w testWriter) Write(p []byte) (int, error) {
	w.t.Log(strings.TrimRight(string(p), "\n"))
	return len(p), nil
}
