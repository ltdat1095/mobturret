package middleware

import (
	"errors"
	"log/slog"
	"net/http"
	"strings"

	"github.com/gin-gonic/gin"

	"mobturret/server/internal/platform/logger"
	"mobturret/server/internal/platform/response"
	"mobturret/server/internal/platform/security"
)

// Context keys used to pass the authenticated user from the
// middleware to the handler. Handlers should use the typed accessors
// below rather than reading these raw.
const (
	ctxUserID    = "auth_user_id"
	ctxUserEmail = "auth_user_email"
)

// JWTMiddleware verifies the Authorization: Bearer <token> header on
// every request in the protected group. On success it stores the
// user_id and email in the gin context; on failure it returns 401.
//
// It calls TokenManager.ValidateToken — no duplicated parsing logic
// (the reference codebase parses the token twice; we don't).
type JWTMiddleware struct {
	tokens *security.TokenManager
	logger *slog.Logger
}

// NewJWTMiddleware wires a TokenManager into a middleware.
func NewJWTMiddleware(tokens *security.TokenManager, logger *slog.Logger) *JWTMiddleware {
	return &JWTMiddleware{tokens: tokens, logger: logger}
}

// Authenticate is the gin.HandlerFunc that verifies the JWT.
func (m *JWTMiddleware) Authenticate() gin.HandlerFunc {
	return func(c *gin.Context) {
		header := c.GetHeader("Authorization")
		if header == "" {
			response.Error(c, http.StatusUnauthorized, "missing Authorization header", nil)
			c.Abort()
			return
		}

		const prefix = "Bearer "
		if !strings.HasPrefix(header, prefix) {
			response.Error(c, http.StatusUnauthorized, "Authorization header must use Bearer scheme", nil)
			c.Abort()
			return
		}

		tokenStr := strings.TrimPrefix(header, prefix)
		claims, err := m.tokens.ValidateToken(tokenStr)
		if err != nil {
			// Don't leak which failure mode (expired vs invalid signature)
			// to the client — both are 401 "invalid token".
			logger.With(c.Request.Context(), m.logger).
				Warn("jwt validation failed",
					slog.String("err", err.Error()),
				)
			response.Error(c, http.StatusUnauthorized, "invalid token", nil)
			c.Abort()
			return
		}

		c.Set(ctxUserID, claims.UserID)
		c.Set(ctxUserEmail, claims.Email)
		c.Next()
	}
}

// UserID retrieves the authenticated user_id from c. Returns "" if not set
// (which means the route wasn't wrapped in JWTMiddleware — caller bug).
func UserID(c *gin.Context) string {
	if v, ok := c.Get(ctxUserID); ok {
		if s, ok := v.(string); ok {
			return s
		}
	}
	return ""
}

// UserEmail retrieves the authenticated user's email from c.
func UserEmail(c *gin.Context) string {
	if v, ok := c.Get(ctxUserEmail); ok {
		if s, ok := v.(string); ok {
			return s
		}
	}
	return ""
}

// IsAuthenticated reports whether the current request was wrapped in
// the JWT middleware. Useful for handlers behind optional-auth groups.
func IsAuthenticated(c *gin.Context) bool {
	_, ok := c.Get(ctxUserID)
	return ok
}

// ErrUnauthenticated is exposed for handler-level tests / helpers that
// want to compare against the canonical "no token" condition.
var ErrUnauthenticated = errors.New("unauthenticated")
