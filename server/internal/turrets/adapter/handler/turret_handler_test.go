package handler_test

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"mobturret/server/internal/platform/middleware"
	"mobturret/server/internal/platform/response"
	"mobturret/server/internal/platform/security"
	"mobturret/server/internal/turrets/adapter/handler"
	"mobturret/server/internal/turrets/usecase"
)

// fakeTurretsUseCase satisfies TurretsUseCaseInterface and lets the
// test control the List response.
type fakeTurretsUseCase struct {
	out usecase.ListOutput
	err error

	lastOwnerID string
}

func (f *fakeTurretsUseCase) List(_ context.Context, ownerID string) (usecase.ListOutput, error) {
	f.lastOwnerID = ownerID
	return f.out, f.err
}

var _ usecase.TurretsUseCaseInterface = (*fakeTurretsUseCase)(nil)

var testTokens = security.New(
	[]byte("test-secret-key-must-be-32-bytes-long-okay"),
	"mobturret-server",
	"mobturret-mobile",
	24,
)

func newTestServer(uc usecase.TurretsUseCaseInterface) *gin.Engine {
	gin.SetMode(gin.TestMode)
	log := slog.New(slog.NewTextHandler(io.Discard, nil))

	h := handler.NewTurretHandler(uc, log)
	jwtMW := middleware.NewJWTMiddleware(testTokens, log)

	engine := gin.New()
	engine.Use(middleware.RequestID())
	protected := engine.Group("")
	protected.Use(jwtMW.Authenticate())
	h.RegisterProtected(protected)
	return engine
}

func get(t *testing.T, engine *gin.Engine, path, bearer string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodGet, path, nil)
	if bearer != "" {
		req.Header.Set("Authorization", "Bearer "+bearer)
	}
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)
	return w
}

func TestList_WithValidToken_200_EmptyArray(t *testing.T) {
	uc := &fakeTurretsUseCase{
		out: usecase.ListOutput{Turrets: []usecase.TurretDTO{}},
	}
	engine := newTestServer(uc)

	token, err := testTokens.GenerateAccessToken("uuid-owner-1", "owner@example.com")
	require.NoError(t, err)

	w := get(t, engine, "/turrets", token)
	require.Equal(t, http.StatusOK, w.Code)

	var env response.APIResponse
	require.NoError(t, json.Unmarshal(w.Body.Bytes(), &env))
	assert.Equal(t, http.StatusOK, env.Code)

	// data is the ListOutput, which contains a "turrets" key with [].
	data, ok := env.Data.(map[string]any)
	require.True(t, ok, "data should be an object, got %T", env.Data)
	turrets, ok := data["turrets"].([]any)
	require.True(t, ok, "data.turrets should be an array, got %T", data["turrets"])
	assert.Equal(t, 0, len(turrets), "stub should return empty array")

	// Wire-format guarantee: the array serializes as `[]`, not `null`.
	raw, _ := json.Marshal(env)
	assert.NotContains(t, string(raw), `"turrets":null`,
		"envelope must not contain null turrets list")

	assert.Equal(t, "uuid-owner-1", uc.lastOwnerID,
		"user_id from JWT must be passed through to the usecase")
}

func TestList_NoToken_401(t *testing.T) {
	uc := &fakeTurretsUseCase{}
	engine := newTestServer(uc)

	w := get(t, engine, "/turrets", "")
	require.Equal(t, http.StatusUnauthorized, w.Code)
}

func TestList_ExpiredToken_401(t *testing.T) {
	expired := security.New(
		[]byte("test-secret-key-must-be-32-bytes-long-okay"),
		"mobturret-server",
		"mobturret-mobile",
		-1, // TTL hours in the past
	)
	token, err := expired.GenerateAccessToken("uuid", "x@y.com")
	require.NoError(t, err)

	uc := &fakeTurretsUseCase{}
	engine := newTestServer(uc)

	w := get(t, engine, "/turrets", token)
	require.Equal(t, http.StatusUnauthorized, w.Code)
}

func TestList_PassesOwnerIDToUseCase(t *testing.T) {
	// The contract: handler reads user_id from JWT context and
	// passes it to the usecase. The stub ignores it today, but M3
	// proper will use it to filter the GSI query.
	uc := &fakeTurretsUseCase{
		out: usecase.ListOutput{Turrets: []usecase.TurretDTO{}},
	}
	engine := newTestServer(uc)

	token, err := testTokens.GenerateAccessToken("specific-owner-id-42", "x@y.com")
	require.NoError(t, err)

	w := get(t, engine, "/turrets", token)
	require.Equal(t, http.StatusOK, w.Code)
	assert.Equal(t, "specific-owner-id-42", uc.lastOwnerID)
}

func TestList_UseCaseError_500(t *testing.T) {
	// When M3 proper lands and the usecase can fail (DDB down, etc.),
	// the handler must surface 500 — not silently return []. This
	// pins the contract for that future branch.
	uc := &fakeTurretsUseCase{err: assertAnError{}}
	engine := newTestServer(uc)

	token, err := testTokens.GenerateAccessToken("uuid", "x@y.com")
	require.NoError(t, err)

	w := get(t, engine, "/turrets", token)
	require.Equal(t, http.StatusInternalServerError, w.Code)
}

// assertAnError is a small wrapper to avoid importing "errors" in
// the test file just for one value.
type assertAnError struct{}

func (assertAnError) Error() string { return "synthetic failure" }

var _ = time.Second // keep "time" import in case future tests need it
