// Package handler contains the Gin HTTP handlers for the auth module.
// Handlers depend on AuthUseCaseInterface — never on *AuthUseCase
// directly — so unit tests can swap in a fake.
package handler

import (
	"errors"
	"log/slog"
	"net/http"

	"github.com/gin-gonic/gin"

	"mobturret/server/internal/auth/domain/entity"
	"mobturret/server/internal/auth/usecase"
	"mobturret/server/internal/platform/logger"
	"mobturret/server/internal/platform/middleware"
	"mobturret/server/internal/platform/response"
)

// AuthHandler exposes Register, Login, Me over HTTP. Each method
// binds the request, calls the usecase, and maps domain errors to
// HTTP status codes via errors.Is against typed sentinels.
type AuthHandler struct {
	uc     usecase.AuthUseCaseInterface
	logger *slog.Logger
}

// NewAuthHandler is the Wire-friendly constructor.
func NewAuthHandler(uc usecase.AuthUseCaseInterface, logger *slog.Logger) *AuthHandler {
	return &AuthHandler{uc: uc, logger: logger}
}

// RegisterPublic wires the unauthenticated endpoints. Caller can
// pass either a *gin.Engine or a *gin.RouterGroup — both satisfy
// gin.IRoutes.
//
//	engine.POST("/auth/signup", h.Register) — equivalent
//	group.POST("/auth/signup", h.Register) — equivalent
func (h *AuthHandler) RegisterPublic(r gin.IRoutes) {
	r.POST("/auth/signup", h.Register)
	r.POST("/auth/login", h.Login)
}

// RegisterProtected wires the authenticated endpoints. Caller passes
// the protected group (already wrapped in JWTMiddleware).
func (h *AuthHandler) RegisterProtected(r gin.IRoutes) {
	r.GET("/auth/me", h.Me)
}

// Register handles POST /auth/signup.
func (h *AuthHandler) Register(c *gin.Context) {
	var in usecase.RegisterInput
	if err := c.ShouldBindJSON(&in); err != nil {
		response.Error(c, http.StatusBadRequest, "invalid request body", err.Error())
		return
	}

	out, err := h.uc.Register(c.Request.Context(), in)
	if err != nil {
		h.mapError(c, err, "register")
		return
	}
	response.Success(c, http.StatusCreated, out)
}

// Login handles POST /auth/login.
func (h *AuthHandler) Login(c *gin.Context) {
	var in usecase.LoginInput
	if err := c.ShouldBindJSON(&in); err != nil {
		response.Error(c, http.StatusBadRequest, "invalid request body", err.Error())
		return
	}

	out, err := h.uc.Login(c.Request.Context(), in)
	if err != nil {
		h.mapError(c, err, "login")
		return
	}
	response.Success(c, http.StatusOK, out)
}

// Me handles GET /auth/me. Requires the JWT middleware to have set
// the user_id; if absent, this is a routing bug and we 500.
func (h *AuthHandler) Me(c *gin.Context) {
	userID := middleware.UserID(c)
	if userID == "" {
		response.Error(c, http.StatusInternalServerError, "auth middleware not applied to this route", nil)
		return
	}

	out, err := h.uc.Me(c.Request.Context(), userID)
	if err != nil {
		h.mapError(c, err, "me")
		return
	}
	response.Success(c, http.StatusOK, out)
}

// mapError converts domain errors into HTTP envelopes. Logging is
// done here so the usecase stays free of HTTP concerns.
func (h *AuthHandler) mapError(c *gin.Context, err error, op string) {
	ctx := c.Request.Context()

	switch {
	case errors.Is(err, entity.ErrEmailAlreadyExists):
		response.Error(c, http.StatusConflict, "email already registered", nil)

	case errors.Is(err, entity.ErrInvalidCredentials):
		// Intentionally vague: don't leak whether email exists.
		response.Error(c, http.StatusUnauthorized, "invalid credentials", nil)

	case errors.Is(err, entity.ErrInvalidInput):
		response.Error(c, http.StatusBadRequest, "invalid input", err.Error())

	case errors.Is(err, entity.ErrUserNotFound):
		// Internal — should not normally surface from a public
		// endpoint, but /me could see it if the user was deleted
		// between token issue and request.
		response.Error(c, http.StatusNotFound, "user not found", nil)

	default:
		logger.With(ctx, h.logger).Error("auth usecase failed",
			slog.String("op", op),
			slog.Any("err", err),
		)
		response.Error(c, http.StatusInternalServerError, "internal error", nil)
	}
}
