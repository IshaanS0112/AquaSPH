// aquactl is the command-line client for the AquaSPH platform.
//
//	export AQUASPH_URL=http://localhost:8080 AQUASPH_API_KEY=aqk_...
//	aquactl submit dam_break --time 0.5 --set /materials/0/viscosity=1.0 --watch
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"text/tabwriter"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/client"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
)

const usage = `aquactl <command> [flags]

Jobs
  scenarios                          list the scenario catalogue
  submit <scenario> [flags]          submit a job (see: aquactl submit -h)
  get <job-id>                       show a job
  list [--state S] [--scenario N] [--sweep ID] [--label k:v] [--limit N]
  watch <job-id>                     follow live progress until the job finishes
  cancel <job-id>                    cancel (a running job keeps its partial result)
  result <job-id>                    print the solver's metrics JSON
  history <job-id>                   state transitions
  artifacts <job-id>                 list output files
  download <job-id> <name> [-o F]    download one output file

Sweeps
  sweep create <scenario> --grid /ptr=v1,v2 [--grid ...] [submit flags]
  sweep get <sweep-id>
  sweep list
  sweep results <sweep-id> [--metrics /a,/b] [--format json|csv]
  sweep cancel <sweep-id>

Environment: AQUASPH_URL (default http://localhost:8080), AQUASPH_API_KEY
`

func die(err error) {
	fmt.Fprintln(os.Stderr, "aquactl:", err)
	os.Exit(1)
}

func printJSON(v any) {
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	_ = enc.Encode(v)
}

type multi []string

func (m *multi) String() string     { return strings.Join(*m, ",") }
func (m *multi) Set(v string) error { *m = append(*m, v); return nil }

// parseValue reads a flag value as JSON when it is valid JSON (numbers, booleans, arrays,
// quoted strings) and as a bare string otherwise, so --set /materials/0/name=oil works without
// shell-quoting quotes.
func parseValue(s string) any {
	var v any
	if err := json.Unmarshal([]byte(s), &v); err == nil {
		return v
	}
	return s
}

type submitFlags struct {
	fs                         *flag.FlagSet
	quality, idemKey, specFile string
	simTime                    float64
	steps, timeout, priority   int
	noCache, watch             bool
	sets, labels               multi
}

func newSubmitFlags(name string) *submitFlags {
	f := &submitFlags{fs: flag.NewFlagSet(name, flag.ExitOnError)}
	f.fs.StringVar(&f.quality, "quality", "low", "low | medium | high")
	f.fs.Float64Var(&f.simTime, "time", 0, "simulated seconds (default: the scenario's own)")
	f.fs.IntVar(&f.steps, "steps", 0, "hard cap on timesteps")
	f.fs.IntVar(&f.timeout, "timeout", 0, "wall-clock limit in seconds (default: tenant limit)")
	f.fs.IntVar(&f.priority, "priority", -1, "0-9")
	f.fs.BoolVar(&f.noCache, "no-cache", false, "always compute, never reuse a cached result")
	f.fs.BoolVar(&f.watch, "watch", false, "follow progress until the job finishes")
	f.fs.StringVar(&f.idemKey, "idempotency-key", "", "make retries of this submission safe")
	f.fs.StringVar(&f.specFile, "spec", "", "submit an inline scenario from this JSON file instead of a named one")
	f.fs.Var(&f.sets, "set", "override: /json/pointer=value (repeatable)")
	f.fs.Var(&f.labels, "label", "label: key=value (repeatable)")
	return f
}

func (f *submitFlags) request(scenarioName string) service.JobRequest {
	req := service.JobRequest{Scenario: scenarioName, Quality: f.quality}
	if f.specFile != "" {
		b, err := os.ReadFile(f.specFile)
		if err != nil {
			die(err)
		}
		if err := json.Unmarshal(b, &req.Spec); err != nil {
			die(fmt.Errorf("%s: %w", f.specFile, err))
		}
		req.Scenario = ""
	}
	if f.simTime > 0 {
		req.SimTime = &f.simTime
	}
	if f.steps > 0 {
		req.MaxSteps = &f.steps
	}
	if f.timeout > 0 {
		req.TimeoutSeconds = &f.timeout
	}
	if f.priority >= 0 {
		req.Priority = &f.priority
	}
	if f.noCache {
		no := false
		req.Cache = &no
	}
	for _, s := range f.sets {
		k, v, ok := strings.Cut(s, "=")
		if !ok {
			die(fmt.Errorf("--set %q: want /pointer=value", s))
		}
		if req.Overrides == nil {
			req.Overrides = map[string]any{}
		}
		req.Overrides[k] = parseValue(v)
	}
	for _, l := range f.labels {
		k, v, ok := strings.Cut(l, "=")
		if !ok {
			die(fmt.Errorf("--label %q: want key=value", l))
		}
		if req.Labels == nil {
			req.Labels = map[string]string{}
		}
		req.Labels[k] = v
	}
	return req
}

// parseInterleaved accepts flags before and after positional arguments.
func parseInterleaved(fs *flag.FlagSet, args []string) []string {
	var pos []string
	for {
		fs.Parse(args)
		args = fs.Args()
		if len(args) == 0 {
			return pos
		}
		pos = append(pos, args[0])
		args = args[1:]
	}
}

func need(pos []string, n int, what string) {
	if len(pos) != n {
		die(fmt.Errorf("expected %s", what))
	}
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	base := os.Getenv("AQUASPH_URL")
	if base == "" {
		base = "http://localhost:8080"
	}
	c := client.New(base, os.Getenv("AQUASPH_API_KEY"))
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	args := os.Args[2:]

	switch os.Args[1] {
	case "scenarios":
		list, err := c.Scenarios(ctx)
		if err != nil {
			die(err)
		}
		tw := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
		fmt.Fprintln(tw, "NAME\tTIER\tDESCRIPTION")
		for _, s := range list {
			fmt.Fprintf(tw, "%s\t%d\t%s\n", s.Name, s.Tier, s.Description)
		}
		tw.Flush()
		fmt.Println("\nTier 1 = physically demonstrable. Tier 2 = visual experiment, not predictive.")

	case "submit":
		f := newSubmitFlags("submit")
		pos := parseInterleaved(f.fs, args)
		name := ""
		if f.specFile == "" {
			need(pos, 1, "a scenario name (or --spec FILE)")
			name = pos[0]
		}
		j, err := c.SubmitJob(ctx, f.request(name), f.idemKey)
		if err != nil {
			die(err)
		}
		note := ""
		if j.CacheHit {
			note = fmt.Sprintf(" (cache hit: result of %s)", *j.SourceJobID)
		}
		fmt.Fprintf(os.Stderr, "job %s %s%s\n", j.ID, j.State, note)
		if f.watch && !j.Terminal() {
			watch(ctx, c, j.ID)
		} else {
			fmt.Println(j.ID)
		}

	case "get":
		need(args, 1, "a job ID")
		j, err := c.GetJob(ctx, args[0])
		if err != nil {
			die(err)
		}
		printJSON(j)

	case "list":
		fs := flag.NewFlagSet("list", flag.ExitOnError)
		state := fs.String("state", "", "filter by state")
		scen := fs.String("scenario", "", "filter by scenario")
		sweep := fs.String("sweep", "", "filter by sweep ID")
		limit := fs.Int("limit", 20, "page size")
		var labels multi
		fs.Var(&labels, "label", "key:value (repeatable)")
		fs.Parse(args)
		q := url.Values{}
		for k, v := range map[string]string{"state": *state, "scenario": *scen, "sweep_id": *sweep} {
			if v != "" {
				q.Set(k, v)
			}
		}
		for _, l := range labels {
			q.Add("label", l)
		}
		q.Set("limit", strconv.Itoa(*limit))
		p, err := c.ListJobs(ctx, q)
		if err != nil {
			die(err)
		}
		tw := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
		fmt.Fprintln(tw, "ID\tSTATE\tOUTCOME\tSCENARIO\tQUALITY\tPROGRESS\tCACHE\tCREATED")
		for _, j := range p.Data {
			outcome, prog := "-", "-"
			if j.Outcome != nil {
				outcome = *j.Outcome
			}
			if j.Progress != nil {
				prog = fmt.Sprintf("%.0f%%", 100*j.Progress.Fraction)
			}
			fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\t%s\t%v\t%s\n", j.ID, j.State, outcome, j.Scenario, j.Quality,
				prog, j.CacheHit, j.CreatedAt.Local().Format("Jan 02 15:04:05"))
		}
		tw.Flush()
		if p.NextCursor != nil {
			fmt.Fprintln(os.Stderr, "(more results; the API's cursor is", *p.NextCursor+")")
		}

	case "watch":
		need(args, 1, "a job ID")
		watch(ctx, c, args[0])

	case "cancel":
		need(args, 1, "a job ID")
		j, err := c.CancelJob(ctx, args[0])
		if err != nil {
			die(err)
		}
		fmt.Printf("%s %s (cancel_requested=%v)\n", j.ID, j.State, j.CancelRequested)

	case "result":
		need(args, 1, "a job ID")
		r, err := c.Result(ctx, args[0])
		if err != nil {
			die(err)
		}
		if r.Partial {
			fmt.Fprintf(os.Stderr, "note: partial result (job %s); metrics describe the run up to where it stopped\n", r.State)
		}
		var v any
		json.Unmarshal(r.Metrics, &v)
		printJSON(v)

	case "history":
		need(args, 1, "a job ID")
		h, err := c.History(ctx, args[0])
		if err != nil {
			die(err)
		}
		var v any
		json.Unmarshal(h, &v)
		printJSON(v)

	case "artifacts":
		need(args, 1, "a job ID")
		list, err := c.Artifacts(ctx, args[0])
		if err != nil {
			die(err)
		}
		tw := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
		fmt.Fprintln(tw, "NAME\tBYTES\tSHA256")
		for _, a := range list {
			fmt.Fprintf(tw, "%s\t%d\t%s\n", a.Name, a.Size, a.SHA256[:16])
		}
		tw.Flush()

	case "download":
		fs := flag.NewFlagSet("download", flag.ExitOnError)
		out := fs.String("o", "", "output file (default: the artifact name)")
		pos := parseInterleaved(fs, args)
		need(pos, 2, "a job ID and an artifact name")
		b, err := c.Download(ctx, pos[0], pos[1])
		if err != nil {
			die(err)
		}
		if *out == "" {
			*out = pos[1]
		}
		if err := os.WriteFile(*out, b, 0o644); err != nil {
			die(err)
		}
		fmt.Fprintf(os.Stderr, "wrote %s (%d bytes)\n", *out, len(b))

	case "sweep":
		sweepCmd(ctx, c, args)

	default:
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
}

func sweepCmd(ctx context.Context, c *client.Client, args []string) {
	if len(args) == 0 {
		die(errors.New("sweep needs a subcommand: create, get, list, results, cancel"))
	}
	sub, args := args[0], args[1:]
	switch sub {
	case "create":
		f := newSubmitFlags("sweep create")
		var grid multi
		f.fs.Var(&grid, "grid", "axis: /json/pointer=v1,v2,... (repeatable)")
		pos := parseInterleaved(f.fs, args)
		need(pos, 1, "a scenario name")
		req := service.SweepRequest{JobRequest: f.request(pos[0]), Grid: map[string][]any{}}
		for _, g := range grid {
			k, vs, ok := strings.Cut(g, "=")
			if !ok {
				die(fmt.Errorf("--grid %q: want /pointer=v1,v2", g))
			}
			for _, v := range strings.Split(vs, ",") {
				req.Grid[k] = append(req.Grid[k], parseValue(v))
			}
		}
		s, err := c.SubmitSweep(ctx, req, f.idemKey)
		if err != nil {
			die(err)
		}
		fmt.Fprintf(os.Stderr, "sweep %s: %d jobs (%d already cached)\n", s.ID, s.Total, s.CacheHits)
		fmt.Println(s.ID)
	case "get":
		need(args, 1, "a sweep ID")
		s, err := c.GetSweep(ctx, args[0])
		if err != nil {
			die(err)
		}
		printJSON(s)
	case "list":
		p, err := c.ListSweeps(ctx)
		if err != nil {
			die(err)
		}
		tw := tabwriter.NewWriter(os.Stdout, 0, 2, 2, ' ', 0)
		fmt.Fprintln(tw, "ID\tSTATE\tSCENARIO\tTOTAL\tDONE\tFAILED\tCACHE HITS")
		for _, s := range p.Data {
			fmt.Fprintf(tw, "%s\t%s\t%s\t%d\t%d\t%d\t%d\n", s.ID, s.State, s.Scenario, s.Total,
				s.Counts["completed"], s.Counts["failed"], s.CacheHits)
		}
		tw.Flush()
	case "results":
		fs := flag.NewFlagSet("sweep results", flag.ExitOnError)
		metrics := fs.String("metrics", "", "comma-separated JSON Pointers into metrics.json")
		format := fs.String("format", "csv", "json | csv")
		pos := parseInterleaved(fs, args)
		need(pos, 1, "a sweep ID")
		var m []string
		if *metrics != "" {
			m = strings.Split(*metrics, ",")
		}
		b, err := c.SweepResults(ctx, pos[0], m, *format)
		if err != nil {
			die(err)
		}
		os.Stdout.Write(b)
	case "cancel":
		need(args, 1, "a sweep ID")
		if err := c.CancelSweep(ctx, args[0]); err != nil {
			die(err)
		}
		fmt.Println("cancelled")
	default:
		die(fmt.Errorf("unknown sweep subcommand %q", sub))
	}
}

// watch renders a one-line progress bar from the SSE stream.
func watch(ctx context.Context, c *client.Client, id string) {
	tty := isTerminal()
	start := time.Now()
	err := c.Watch(ctx, id, func(ev client.Event) {
		switch ev.Name {
		case "progress":
			var p client.Progress
			if json.Unmarshal(ev.Data, &p) != nil {
				return
			}
			const width = 30
			n := int(p.Fraction*width + 0.5)
			line := fmt.Sprintf("[%s%s] %5.1f%%  t=%.3f/%.3f s  step %d  fluid %d",
				strings.Repeat("#", n), strings.Repeat(".", width-n), 100*p.Fraction, p.T, p.TEnd, p.Step, p.Fluid)
			if tty {
				fmt.Fprintf(os.Stderr, "\r%s", line)
			} else {
				fmt.Fprintln(os.Stderr, line)
			}
		case "state":
			var s struct{ State string }
			json.Unmarshal(ev.Data, &s)
			if tty {
				fmt.Fprintln(os.Stderr)
			}
			fmt.Fprintf(os.Stderr, "-> %s\n", s.State)
		case "done":
			var j client.Job
			json.Unmarshal(ev.Data, &j)
			if tty {
				fmt.Fprintln(os.Stderr)
			}
			summary := j.State
			if j.Outcome != nil {
				summary += " (" + *j.Outcome + ")"
			}
			if j.Error != nil {
				summary += ": " + j.Error.Code + " " + j.Error.Message
			}
			fmt.Fprintf(os.Stderr, "job %s %s after %s\n", j.ID, summary, time.Since(start).Round(time.Millisecond))
			fmt.Println(j.ID)
		}
	})
	if err != nil {
		die(err)
	}
}

func isTerminal() bool {
	fi, err := os.Stderr.Stat()
	return err == nil && fi.Mode()&os.ModeCharDevice != 0
}
