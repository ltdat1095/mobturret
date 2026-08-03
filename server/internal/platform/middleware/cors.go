// Package middleware contains the project's standard Gin middleware:
// CORS, request_id, JWT auth, and (later) rate limiting.
package middleware

import (
	"github.com/gin-gonic/gin"
)

// CORS returns a permissive CORS middleware for local dev. In
// production this is replaced by a tighter allowlist — Phase 2 wires
// API Gateway as the actual CORS front door, so this middleware
// disappears then.
//
// Permissive means:
//   - Allow any origin (Flutter mobile/web, Android emulator, curl)
//   - Allow common methods
//   - Allow Authorization + Content-Type headers
//   - Short-circuit OPTIONS preflight with 204
func CORS() gin.HandlerFunc {
	return func(c *gin.Context) {
		c.Header("Access-Control-Allow-Origin", "*")
		c.Header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
		c.Header("Access-Control-Allow-Headers", "Origin, Content-Type, Accept, Authorization, X-Request-Id")
		c.Header("Access-Control-Expose-Headers", "X-Request-Id")
		c.Header("Access-Control-Max-Age", "86400")

		if c.Request.Method == "OPTIONS" {
			c.AbortWithStatus(204)
			return
		}
		c.Next()
	}
}
