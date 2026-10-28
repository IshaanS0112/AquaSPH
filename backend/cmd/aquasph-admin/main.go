// aquasph-admin is the operator tool: migrations, tenants, API keys, workers and queue state.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"strings"
	"text/tabwriter"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/db"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/queue"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
)

const usage = `aquasph-admin <command> [flags]

  migrate                                   apply pending schema migrations
  tenant create <name> [limit flags]        create a tenant
  tenant update <name> [limit flags]        change a tenant's limits
  tenant list                               list tenants and their limits
  key create <tenant> [--name label]        mint an API key (printed once)
  key list <tenant>                         list a tenant's keys
  key revoke <prefix>                       revoke a key immediately
  workers [--all]                           list live (or all) workers
  queue                                     job counts and oldest wait

limit flags: --max-concurrent N --max-queued N --max-particles N
             --max-wall-seconds N --rate-per-minute N --rate-burst N
`

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "aquasph-admin: "+format+"\n", args...)
	os.Exit(1)
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	url := os.Getenv("AQUASPH_DATABASE_URL")
	if url == "" {
		fail("AQUASPH_DATABASE_URL is not set")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	pool, err := db.Connect(ctx, url, 2)
	if err != nil {
		fail("%v", err)
	}
	defer pool.Close()
	st := store.New(pool)

	cmd := strings.Join(os.Args[1:min(3, len(os.Args))], " ")
	switch {
	case os.Args[1] == "migrate":
		ran, err := db.Migrate(ctx, pool)
		if err != nil {
			fail("%v", err)
		}
		fmt.Printf("applied %d migration(s) %v\n", len(ran), ran)
	case cmd == "tenant create" || cmd == "tenant update":
		name, limits := tenantArgs(os.Args[3:])
		var t *domain.Tenant
		if os.Args[2] == "create" {
			t, err = st.CreateTenant(ctx, name, limits)
		} else {
			t, err = st.UpdateTenantLimits(ctx, name, limits)
		}
		if err != nil {
			fail("%v", err)
		}
		printTenants([]*domain.Tenant{t})
	case cmd == "tenant list":
		ts, err := st.ListTenants(ctx)
		if err != nil {
			fail("%v", err)
		}
		printTenants(ts)
	case cmd == "key create":
		fs := flag.NewFlagSet("key create", flag.ExitOnError)
		label := fs.String("name", "", "label for the key")
		tenantName := positional(fs, os.Args[3:], "tenant")
		t, err := st.GetTenantByName(ctx, tenantName)
		if err != nil {
			fail("tenant %q: %v", tenantName, err)
		}
		plain, k, err := st.CreateAPIKey(ctx, t.ID, *label)
		if err != nil {
			fail("%v", err)
		}
		fmt.Fprintf(os.Stderr, "Created key %s for tenant %s. It is shown once; store it now.\n", k.Prefix, t.Name)
		fmt.Println(plain)
	case cmd == "key list":
		fs := flag.NewFlagSet("key list", flag.ExitOnError)
		t, err := st.GetTenantByName(ctx, positional(fs, os.Args[3:], "tenant"))
		if err != nil {
			fail("%v", err)
		}
		keys, err := st.ListAPIKeys(ctx, t.ID)
		if err != nil {
			fail("%v", err)
		}
		tw := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
		fmt.Fprintln(tw, "PREFIX\tNAME\tCREATED\tLAST USED\tREVOKED")
		for _, k := range keys {
			fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\n", k.Prefix, k.Name, k.CreatedAt.Format(time.RFC3339),
				ts(k.LastUsedAt), ts(k.RevokedAt))
		}
		tw.Flush()
	case cmd == "key revoke":
		fs := flag.NewFlagSet("key revoke", flag.ExitOnError)
		prefix := positional(fs, os.Args[3:], "prefix")
		if err := st.RevokeAPIKey(ctx, prefix); err != nil {
			fail("revoke %s: %v", prefix, err)
		}
		fmt.Printf("revoked %s\n", prefix)
	case os.Args[1] == "workers":
		fs := flag.NewFlagSet("workers", flag.ExitOnError)
		all := fs.Bool("all", false, "include stopped workers")
		fs.Parse(os.Args[2:])
		ws, err := st.ListWorkers(ctx, *all)
		if err != nil {
			fail("%v", err)
		}
		tw := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
		fmt.Fprintln(tw, "ID\tHOST\tPID\tSOLVER\tSLOTS\tLAST HEARTBEAT\tSTOPPED")
		for _, w := range ws {
			fmt.Fprintf(tw, "%s\t%s\t%d\t%s\t%d\t%s ago\t%s\n", w.ID, w.Hostname, w.PID, w.SolverID[:12],
				w.Concurrency, time.Since(w.HeartbeatAt).Round(time.Second), ts(w.StoppedAt))
		}
		tw.Flush()
	case os.Args[1] == "queue":
		q := queue.New(pool, queue.Config{})
		depth, err := q.Depth(ctx)
		if err != nil {
			fail("%v", err)
		}
		age, _ := q.OldestQueuedAge(ctx)
		for _, s := range domain.AllStates {
			fmt.Printf("%-10s %d\n", s, depth[s])
		}
		fmt.Printf("oldest claimable job has waited %s\n", age.Round(time.Millisecond))
	default:
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
}

// positional parses flags that may come before or after one required
// positional argument.
func positional(fs *flag.FlagSet, args []string, what string) string {
	var pos []string
	for len(args) > 0 {
		fs.Parse(args)
		args = fs.Args()
		if len(args) > 0 {
			pos = append(pos, args[0])
			args = args[1:]
		}
	}
	if len(pos) != 1 {
		fail("expected exactly one %s argument", what)
	}
	return pos[0]
}

func tenantArgs(args []string) (string, store.TenantLimits) {
	fs := flag.NewFlagSet("tenant", flag.ExitOnError)
	var l store.TenantLimits
	intFlag := func(name, help string, dst **int) {
		fs.Func(name, help, func(s string) error {
			var v int
			if _, err := fmt.Sscan(s, &v); err != nil || v <= 0 {
				return fmt.Errorf("must be a positive integer")
			}
			*dst = &v
			return nil
		})
	}
	intFlag("max-concurrent", "jobs running at once", &l.MaxConcurrentJobs)
	intFlag("max-queued", "jobs waiting at once", &l.MaxQueuedJobs)
	intFlag("max-particles", "particle ceiling per job", &l.MaxParticles)
	intFlag("max-wall-seconds", "wall-clock limit per job", &l.MaxWallSeconds)
	intFlag("rate-per-minute", "sustained request rate", &l.RatePerMinute)
	intFlag("rate-burst", "request burst size", &l.RateBurst)
	return positional(fs, args, "tenant name"), l
}

func printTenants(ts []*domain.Tenant) {
	tw := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
	fmt.Fprintln(tw, "NAME\tCONCURRENT\tQUEUED\tPARTICLES\tWALL S\tRATE/MIN\tBURST\tID")
	for _, t := range ts {
		fmt.Fprintf(tw, "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n", t.Name, t.MaxConcurrentJobs, t.MaxQueuedJobs,
			t.MaxParticles, t.MaxWallSeconds, t.RatePerMinute, t.RateBurst, t.ID)
	}
	tw.Flush()
}

func ts(t *time.Time) string {
	if t == nil {
		return "-"
	}
	return t.Format(time.RFC3339)
}
