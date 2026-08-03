package middleware

import (
	"github.com/gin-gonic/gin"
	"github.com/google/uuid"

	"mobturret/server/internal/platform/logger"
)

// RequestID assigns a UUID to every incoming request. The id is:
//   - read from the X-Request-Id header if present (caller-side
//     tracing), or generated fresh otherwise
//   - echoed in the X-Request-Id response header
//   - stored in gin.Context under key "request_id"
//   - attached to the request's context.Context so downstream
//     logger.With(ctx, base) can pick it up
//
// Reference gap closed: the reference codebase has no request_id
// middleware, so log correlation across requests was impossible.
func RequestID() gin.HandlerFunc {
	return func(c *gin.Context) {
		id := c.GetHeader("X-Request-Id")
		if id == "" {
			id = uuid.NewString()
		}

		c.Header("X-Request-Id", id)
		c.Set("request_id", id)

		// Propagate to request context so handlers/usecases can
		// attach the id to slog calls.
		ctx := logger.WithRequestID(c.Request.Context(), id)
		c.Request = c.Request.WithContext(ctx)

		c.Next()
	}
}

// GetRequestID extracts the request_id from a gin.Context. Returns "" if absent.
func GetRequestID(c *gin.Context) string {
	if v, ok := c.Get("request_id"); ok {
		if s, ok := v.(string); ok {
			return s
		}
	}
	return ""
}
