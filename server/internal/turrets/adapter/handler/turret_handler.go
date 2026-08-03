// Package handler contains the Gin HTTP handlers for the turrets
// module. Mirrors the auth handler's structure: depends on a
// TurretsUseCaseInterface so tests can swap in a fake.
package handler

import (
	"log/slog"
	"net/http"

	"github.com/gin-gonic/gin"

	"mobturret/server/internal/platform/middleware"
	"mobturret/server/internal/platform/response"
	"mobturret/server/internal/turrets/usecase"
)

// TurretHandler exposes turrets endpoints over HTTP. For the M3
// stub, the only protected route is GET /turrets; M3 proper adds
// GET /turrets/:id, POST /turrets, and POST /turrets/:id/commands.
type TurretHandler struct {
	uc     usecase.TurretsUseCaseInterface
	logger *slog.Logger
}

// NewTurretHandler is the Wire-friendly constructor.
func NewTurretHandler(uc usecase.TurretsUseCaseInterface, logger *slog.Logger) *TurretHandler {
	return &TurretHandler{uc: uc, logger: logger}
}

// RegisterProtected wires the JWT-authenticated endpoints. Caller
// passes a group already wrapped in JWTMiddleware.
func (h *TurretHandler) RegisterProtected(r gin.IRoutes) {
	r.GET("/turrets", h.List)
}

// List handles GET /turrets. Returns the current user's turrets;
// the stub returns an empty list. JWT middleware has already set
// the user_id on the context.
func (h *TurretHandler) List(c *gin.Context) {
	ownerID := middleware.UserID(c)
	if ownerID == "" {
		// Routing bug: this handler was mounted outside the JWT group.
		response.Error(c, http.StatusInternalServerError,
			"auth middleware not applied to this route", nil)
		return
	}

	out, err := h.uc.List(c.Request.Context(), ownerID)
	if err != nil {
		// The stub never errors. When M3 proper lands, this branch
		// will map ErrRobotNotFound / ErrUnauthorized to 404 / 403.
		h.logger.ErrorContext(c.Request.Context(),
			"turrets list failed",
			slog.Any("err", err),
			slog.String("owner_id", ownerID),
		)
		response.Error(c, http.StatusInternalServerError,
			"internal error", nil)
		return
	}

	// Always return the list under a "data" envelope so the Flutter
	// client's `response.data['data']` pattern keeps working.
	c.JSON(http.StatusOK, response.APIResponse{
		Code:    http.StatusOK,
		Message: http.StatusText(http.StatusOK),
		Data:    out,
	})
}
