package db_test

import (
	"context"
	"strings"
	"sync"
	"testing"

	"github.com/IshaanS0112/AquaSPH/backend/internal/db"
	"github.com/IshaanS0112/AquaSPH/backend/internal/testutil"
)

func TestMigrateIsIdempotent(t *testing.T) {
	pool, _ := testutil.NewDB(t) // already migrated once
	ran, err := db.Migrate(context.Background(), pool)
	if err != nil || len(ran) != 0 {
		t.Fatalf("second migrate ran %v, err %v", ran, err)
	}
}

// Two replicas starting together must not both apply a migration. The
// advisory lock makes the second wait and then find nothing to do.
func TestConcurrentMigratorsApplyEachMigrationOnce(t *testing.T) {
	pool, _ := testutil.NewDB(t)
	ctx := context.Background()
	if _, err := pool.Exec(ctx, `DROP SCHEMA public CASCADE; CREATE SCHEMA public`); err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	results := make([][]string, 4)
	errs := make([]error, 4)
	for i := range results {
		wg.Add(1)
		go func() {
			defer wg.Done()
			results[i], errs[i] = db.Migrate(ctx, pool)
		}()
	}
	wg.Wait()
	total := 0
	for i := range results {
		if errs[i] != nil {
			t.Fatalf("migrator %d: %v", i, errs[i])
		}
		total += len(results[i])
	}
	all, _ := db.Migrations()
	if total != len(all) {
		t.Fatalf("migrations applied %d times in total, want %d", total, len(all))
	}
}

func TestMigrateRefusesAnEditedMigration(t *testing.T) {
	pool, _ := testutil.NewDB(t)
	ctx := context.Background()
	pool.Exec(ctx, `UPDATE schema_migrations SET checksum = 'deadbeefdeadbeef' WHERE version = '0001_init'`)
	_, err := db.Migrate(ctx, pool)
	if err == nil || !strings.Contains(err.Error(), "modified after being applied") {
		t.Fatalf("expected a checksum error, got %v", err)
	}
}
