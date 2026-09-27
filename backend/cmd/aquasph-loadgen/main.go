// aquasph-loadgen measures the API's own overhead, not the solver's.
//
// Modes:
//
//	get     GET /v1/jobs/{id} for one existing job: auth + rate limit + one indexed read
//	submit  POST /v1/jobs with a request that is already cached: the full submit path
//	        (auth, rate limit, validation, JSON Pointer resolution, canonical hashing,
//	        cache lookup, transactional insert) with zero solver time
//	list    GET /v1/jobs?limit=20: keyset pagination
//
// It prints what it measured and nothing it did not. Use a tenant whose
// rate limit is high enough, or you are measuring the 429 path.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/client"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
)

func main() {
	base := flag.String("url", envOr("AQUASPH_URL", "http://localhost:8080"), "API base URL")
	key := flag.String("key", os.Getenv("AQUASPH_API_KEY"), "API key")
	mode := flag.String("mode", "get", "get | submit | list")
	conc := flag.Int("c", 16, "concurrent clients")
	dur := flag.Duration("d", 20*time.Second, "measurement duration")
	warm := flag.Duration("warmup", 2*time.Second, "warm-up (not measured)")
	scen := flag.String("scenario", "dam_break", "scenario for the seed job")
	flag.Parse()

	c := client.New(*base, *key)
	c.HTTP.Transport = nil // default transport; keep-alive on
	ctx := context.Background()

	simTime := 0.01
	seedReq := service.JobRequest{Scenario: *scen, Quality: "low", SimTime: &simTime}
	fmt.Fprintf(os.Stderr, "seeding: submitting one %s job and waiting for a worker to finish it...\n", *scen)
	seed, err := c.SubmitJob(ctx, seedReq, "")
	if err != nil {
		fail(err)
	}
	wctx, cancel := context.WithTimeout(ctx, 5*time.Minute)
	seed, err = c.WaitFor(wctx, seed.ID, 250*time.Millisecond)
	cancel()
	if err != nil {
		fail(fmt.Errorf("seed job did not finish (is a worker running?): %w", err))
	}
	if seed.State != "completed" {
		fail(fmt.Errorf("seed job ended %s", seed.State))
	}

	var op func() (int, error)
	switch *mode {
	case "get":
		op = func() (int, error) {
			r, err := c.Do(ctx, "GET", "/v1/jobs/"+seed.ID, nil, nil)
			return status(r, err)
		}
	case "submit":
		// Confirm the cache path first, or the benchmark would silently
		// enqueue thousands of real simulations.
		probe, err := c.SubmitJob(ctx, seedReq, "")
		if err != nil || !probe.CacheHit {
			fail(fmt.Errorf("the seed request is not being served from cache (hit=%v, err=%v); refusing to flood the queue",
				probe != nil && probe.CacheHit, err))
		}
		op = func() (int, error) {
			r, err := c.Do(ctx, "POST", "/v1/jobs", seedReq, nil)
			return status(r, err)
		}
	case "list":
		op = func() (int, error) {
			r, err := c.Do(ctx, "GET", "/v1/jobs?limit=20", nil, nil)
			return status(r, err)
		}
	default:
		fail(fmt.Errorf("unknown mode %q", *mode))
	}

	fmt.Fprintf(os.Stderr, "mode=%s clients=%d warmup=%s duration=%s\n", *mode, *conc, *warm, *dur)
	var measuring atomic.Bool
	var mu sync.Mutex
	var lat []time.Duration
	codes := map[int]int{}
	var errs atomic.Int64
	stop := time.Now().Add(*warm + *dur)
	go func() { time.Sleep(*warm); measuring.Store(true) }()

	var wg sync.WaitGroup
	for i := 0; i < *conc; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			local := make([]time.Duration, 0, 4096)
			localCodes := map[int]int{}
			for time.Now().Before(stop) {
				t0 := time.Now()
				code, err := op()
				d := time.Since(t0)
				if !measuring.Load() {
					continue
				}
				if err != nil {
					errs.Add(1)
					continue
				}
				localCodes[code]++
				local = append(local, d)
			}
			mu.Lock()
			lat = append(lat, local...)
			for k, v := range localCodes {
				codes[k] += v
			}
			mu.Unlock()
		}()
	}
	wg.Wait()

	sort.Slice(lat, func(i, j int) bool { return lat[i] < lat[j] })
	pct := func(p float64) time.Duration {
		if len(lat) == 0 {
			return 0
		}
		return lat[min(len(lat)-1, int(p*float64(len(lat))))]
	}
	fmt.Printf("mode:        %s\nclients:     %d\nduration:    %s (after %s warm-up)\n", *mode, *conc, *dur, *warm)
	fmt.Printf("requests:    %d  (%.0f req/s)\n", len(lat), float64(len(lat))/dur.Seconds())
	fmt.Printf("status:      %v\ntransport errors: %d\n", codes, errs.Load())
	fmt.Printf("latency:     p50 %s  p90 %s  p99 %s  max %s\n",
		pct(0.50).Round(10*time.Microsecond), pct(0.90).Round(10*time.Microsecond),
		pct(0.99).Round(10*time.Microsecond), pct(1).Round(10*time.Microsecond))
}

func status(r *client.Response, err error) (int, error) {
	if err != nil {
		return 0, err
	}
	return r.Status, nil
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "aquasph-loadgen:", err)
	os.Exit(1)
}
