package ratelimit

import (
	"context"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/testutil"
	"github.com/redis/go-redis/v9"
)

func limiter(t *testing.T) (*Redis, string) {
	opts, _ := redis.ParseURL(testutil.RedisURL(t))
	c := redis.NewClient(opts)
	t.Cleanup(func() { c.Close() })
	return NewRedis(c, "test:rl:"), testutil.Unique("k")
}

func TestBurstThenDenyThenRecover(t *testing.T) {
	l, key := limiter(t)
	ctx := context.Background()
	// 600/min = one request per 100 ms sustained, bursts of 3.
	for i := 0; i < 3; i++ {
		d, err := l.Allow(ctx, key, 600, 3)
		if err != nil || !d.Allowed || d.Remaining != 2-i {
			t.Fatalf("request %d: %+v %v", i, d, err)
		}
	}
	d, _ := l.Allow(ctx, key, 600, 3)
	if d.Allowed || d.RetryAfter <= 0 || d.RetryAfter > 100*time.Millisecond {
		t.Fatalf("4th request in the burst: %+v", d)
	}
	time.Sleep(d.RetryAfter + 5*time.Millisecond)
	if d, _ := l.Allow(ctx, key, 600, 3); !d.Allowed {
		t.Fatalf("not allowed after waiting RetryAfter: %+v", d)
	}
}

func TestKeysAreIndependent(t *testing.T) {
	l, key := limiter(t)
	ctx := context.Background()
	l.Allow(ctx, key, 60, 1)
	if d, _ := l.Allow(ctx, key+"-other", 60, 1); !d.Allowed {
		t.Fatal("one tenant's usage limited another")
	}
}

// The script is atomic: 50 concurrent requests against a burst of 10
// admit exactly 10, never 11 through a read-modify-write race.
func TestConcurrentRequestsNeverExceedTheBurst(t *testing.T) {
	l, key := limiter(t)
	var allowed atomic.Int64
	var wg sync.WaitGroup
	for i := 0; i < 50; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			d, err := l.Allow(context.Background(), key, 1, 10) // 1/min: no refill during the test
			if err != nil {
				t.Error(err)
			}
			if d.Allowed {
				allowed.Add(1)
			}
		}()
	}
	wg.Wait()
	if allowed.Load() != 10 {
		t.Fatalf("admitted %d, want 10", allowed.Load())
	}
}
