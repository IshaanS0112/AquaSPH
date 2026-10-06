package queue

import (
	"context"
	"log/slog"
	"time"

	"github.com/jackc/pgx/v5"
)

// Listen keeps a dedicated connection LISTENing on NotifyChannel and signals wake
// (non-blocking, coalescing) on every notification.
func Listen(ctx context.Context, url string, wake chan<- struct{}, log *slog.Logger) {
	delay := 100 * time.Millisecond
	for ctx.Err() == nil {
		err := listenOnce(ctx, url, wake)
		if ctx.Err() != nil {
			return
		}
		log.Warn("notify listener disconnected; polling continues", "err", err, "retry_in", delay)
		select {
		case <-ctx.Done():
			return
		case <-time.After(delay):
		}
		delay = min(delay*2, 10*time.Second)
	}
}

func listenOnce(ctx context.Context, url string, wake chan<- struct{}) error {
	conn, err := pgx.Connect(ctx, url)
	if err != nil {
		return err
	}
	defer conn.Close(context.WithoutCancel(ctx))
	if _, err := conn.Exec(ctx, "LISTEN "+NotifyChannel); err != nil {
		return err
	}
	// Wake once after (re)connecting: anything enqueued while we were
	// disconnected produced a notification nobody heard.
	signal(wake)
	for {
		if _, err := conn.WaitForNotification(ctx); err != nil {
			return err
		}
		signal(wake)
	}
}

func signal(wake chan<- struct{}) {
	select {
	case wake <- struct{}{}:
	default:
	}
}
