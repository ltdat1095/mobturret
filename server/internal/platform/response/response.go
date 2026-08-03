// Package response is the project's standard JSON envelope. Every
// handler returns Success or Error — never raw c.JSON — so the wire
// format is uniform across endpoints.
//
// Envelope:
//
//	{
//	  "code":    <int>,             // HTTP status, duplicated for client convenience
//	  "message": "<string>",         // short human-readable summary
//	  "data":    <object|array|null> // present on success
//	  "details": <object|string|null> // present on error (e.g. validation breakdown)
//	}
package response

import (
	"net/http"

	"github.com/gin-gonic/gin"
)

// APIResponse is the envelope. Data and Details are omitempty so
// successful responses don't carry a null "details" and error
// responses don't carry a null "data".
type APIResponse struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
	Data    any    `json:"data,omitempty"`
	Details any    `json:"details,omitempty"`
}

// Success writes a 2xx envelope with the given data payload. code is
// the HTTP status (200, 201, 204).
func Success(c *gin.Context, code int, data any) {
	c.JSON(code, APIResponse{
		Code:    code,
		Message: http.StatusText(code),
		Data:    data,
	})
}

// Error writes a non-2xx envelope. code is the HTTP status (400, 401,
// 409, 500, …). msg is a short client-facing summary; details is an
// optional breakdown (e.g. validation field errors) that the client
// can render. Pass nil for details when there's nothing useful to add.
func Error(c *gin.Context, code int, msg string, details any) {
	c.JSON(code, APIResponse{
		Code:    code,
		Message: msg,
		Details: details,
	})
}
