package api

import (
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/events"
)

// jobEvents streams a job's progress as Server-Sent Events:
//
//	snapshot  the full job, once, on connect
//	progress  solver progress (via Redis when available, else polled)
//	state     a state transition
//	done      the final job representation; the stream then closes
//
// Correctness never depends on Redis: the handler subscribes *before*
// reading the snapshot (so nothing published in between is missed) and
// polls Postgres, the source of truth, on a timer regardless. A lost
// pub/sub message costs one progress update, never a stuck client.
func (s *Server) jobEvents(w http.ResponseWriter, r *http.Request) {
	id, ok := s.pathID(w, r)
	if !ok {
		return
	}
	tenant := tenantFrom(r.Context())
	if _, err := s.store.GetJob(r.Context(), tenant.ID, id); err != nil {
		s.writeError(w, r, err)
		return
	}
	rc := http.NewResponseController(w)
	_ = rc.SetWriteDeadline(time.Time{}) // lift the server's WriteTimeout for this stream only

	var sub <-chan events.Event
	if s.subscriber != nil {
		ch, unsub := s.subscriber.Subscribe(r.Context(), id)
		defer unsub()
		sub = ch
	}
	job, err := s.store.GetJob(r.Context(), tenant.ID, id)
	if err != nil {
		s.writeError(w, r, err)
		return
	}

	h := w.Header()
	h.Set("Content-Type", "text/event-stream")
	h.Set("Cache-Control", "no-cache")
	h.Set("X-Accel-Buffering", "no") // stop nginx-style proxies from buffering the stream
	w.WriteHeader(http.StatusOK)
	s.metrics.SSEStreams.Inc()
	defer s.metrics.SSEStreams.Dec()

	seq := 0
	send := func(event string, v any) bool {
		b, err := json.Marshal(v)
		if err != nil {
			return false
		}
		seq++
		if _, err := fmt.Fprintf(w, "id: %d\nevent: %s\ndata: %s\n\n", seq, event, b); err != nil {
			return false
		}
		return rc.Flush() == nil
	}

	if !send("snapshot", toJobResource(job, false)) {
		return
	}
	if job.State.Terminal() {
		send("done", toJobResource(job, false))
		return
	}
	lastState := job.State
	lastProgress := string(job.Progress)

	// refresh re-reads the job and emits what changed. It reports whether
	// the stream is finished.
	refresh := func() bool {
		j, err := s.store.GetJob(r.Context(), tenant.ID, id)
		if err != nil {
			return true
		}
		if j.State != lastState {
			lastState = j.State
			if !send("state", map[string]any{"state": j.State}) {
				return true
			}
		}
		if sub == nil && string(j.Progress) != lastProgress && len(j.Progress) > 0 {
			lastProgress = string(j.Progress)
			if !send("progress", json.RawMessage(j.Progress)) {
				return true
			}
		}
		if j.State.Terminal() {
			send("done", toJobResource(j, false))
			return true
		}
		return false
	}

	poll := time.NewTicker(s.ssePoll)
	defer poll.Stop()
	ping := time.NewTicker(s.ssePing)
	defer ping.Stop()
	limit := time.NewTimer(s.sseMax)
	defer limit.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case <-limit.C:
			fmt.Fprint(w, ": stream duration limit reached; reconnect to resume\n\n")
			rc.Flush()
			return
		case <-ping.C:
			// A comment line keeps idle connections alive through proxies
			// and detects clients that went away.
			if _, err := fmt.Fprint(w, ": ping\n\n"); err != nil || rc.Flush() != nil {
				return
			}
		case e, ok := <-sub:
			if !ok {
				sub = nil // Redis went away: carry on polling
				continue
			}
			switch e.Type {
			case "progress":
				if e.Progress != nil && !send("progress", e.Progress) {
					return
				}
			case "state":
				if refresh() {
					return
				}
			}
		case <-poll.C:
			if refresh() {
				return
			}
		}
	}
}
