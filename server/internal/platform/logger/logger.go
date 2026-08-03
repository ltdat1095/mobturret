// Package logger creates the process-wide *slog.Logger configured per
// LogConfig. slog is the Go 1.21+ stdlib structured logger — replaces
// the reference codebase's zap dependency with no extra dep.
//
// Usage:
//
//	logger.New(cfg.Log)        // build the logger
//	slog.SetDefault(logger)    // (in main) so platform code can call slog.Info directly
//	logger.WithRequestID(ctx, id) // attach request_id to a context
package logger

import (
	"context"
	"io"
	"log/slog"
	"os"
	"strings"

	"mobturret/server/internal/platform/config"
)

// requestIDKey is the context key for the per-request UUID. Private
// to avoid collisions with other packages.
type requestIDKey struct{}

// New builds a *slog.Logger per LogConfig. Output is stdout.
func New(cfg config.LogConfig) *slog.Logger {
	return newWithWriter(cfg, os.Stdout)
}

// newWithWriter is the testable form of New — lets tests capture log output.
func newWithWriter(cfg config.LogConfig, w io.Writer) *slog.Logger {
	level := parseLevel(cfg.Level)

	opts := &slog.HandlerOptions{
		Level: level,
	}

	var handler slog.Handler
	switch strings.ToLower(cfg.Format) {
	case "json":
		handler = slog.NewJSONHandler(w, opts)
	default: // "console" or anything unrecognized
		handler = slog.NewTextHandler(w, opts)
	}

	return slog.New(handler)
}

func parseLevel(s string) slog.Level {
	switch strings.ToLower(s) {
	case "debug":
		return slog.LevelDebug
	case "warn", "warning":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

// WithRequestID returns a new context with the request_id attached.
// Downstream slog.* calls can read it via FromContext.
func WithRequestID(ctx context.Context, id string) context.Context {
	return context.WithValue(ctx, requestIDKey{}, id)
}

// FromContext returns the request_id stored in ctx, or "" if none.
func FromContext(ctx context.Context) string {
	if v, ok := ctx.Value(requestIDKey{}).(string); ok {
		return v
	}
	return ""
}

// With returns a logger pre-tagged with the request_id from ctx (if any).
// Use this in middleware so downstream handlers get correlated logs.
//
//	logger.FromContext(ctx) returns ""  → returns the base logger
//	logger.FromContext(ctx) returns "…" → returns logger.With("request_id", id)
func With(ctx context.Context, base *slog.Logger) *slog.Logger {
	id := FromContext(ctx)
	if id == "" {
		return base
	}
	return base.With("request_id", id)
}
