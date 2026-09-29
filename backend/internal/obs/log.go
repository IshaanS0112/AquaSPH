// Package obs holds logging and metrics setup shared by the binaries.
package obs

import (
	"io"
	"log/slog"
)

// NewLogger returns a structured logger.
func NewLogger(w io.Writer, level slog.Level, format, service string) *slog.Logger {
	opts := &slog.HandlerOptions{Level: level}
	var h slog.Handler
	if format == "text" {
		h = slog.NewTextHandler(w, opts)
	} else {
		h = slog.NewJSONHandler(w, opts)
	}
	return slog.New(h).With("service", service)
}
