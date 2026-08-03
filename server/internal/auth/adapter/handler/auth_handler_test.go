package handler_test

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/golang-jwt/jwt/v5"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"mobturret/server/internal/auth/adapter/handler"
	"mobturret/server/internal/auth/domain/entity"
	"mobturret/server/internal/auth/usecase"
	"mobturret/server/internal/platform/middleware"
	"mobturret/server/internal/platform/response"
	"mobturret/server/internal/platform/security"
)

// fakeUseCase lets us drive Register / Login / Me outcomes from the
// test without going through a real repo. Implements
// usecase.AuthUseCaseInterface.
type fakeUseCase struct {
	registerOut usecase.RegisterOutput
	registerErr error
	loginOut    usecase.LoginOutput
	loginErr    error
	meOut       usecase.MeOutput
	meErr       error

	// capture calls so tests can assert
	lastRegisterIn usecase.RegisterInput
	lastLoginIn    usecase.LoginInput
	lastMeUserID   string
}

func (f *fakeUseCase) Register(_ context.Context, in usecase.RegisterInput) (usecase.RegisterOutput, error) {
	f.lastRegisterIn = in
	return f.registerOut, f.registerErr
}

func (f *fakeUseCase) Login(_ context.Context, in usecase.LoginInput) (usecase.LoginOutput, error) {
	f.lastLoginIn = in
	return f.loginOut, f.loginErr
}

func (f *fakeUseCase) Me(_ context.Context, userID string) (usecase.MeOutput, error) {
	f.lastMeUserID = userID
	return f.meOut, f.meErr
}

// Compile-time interface check.
var _ usecase.AuthUseCaseInterface = (*fakeUseCase)(nil)

// testTokens is a TokenManager configured with a fixed secret so the
// tests can both sign and verify.
var testTokens = security.New(
	[]byte("test-secret-key-must-be-32-bytes-long-okay"),
	"mobturret-server",
	"mobturret-mobile",
	24,
)

// newTestServer wires the handler + middleware into a Gin engine
// identical to the production build (without the rest of the app).
func newTestServer(uc usecase.AuthUseCaseInterface) *gin.Engine {
	gin.SetMode(gin.TestMode)
	log := slog.New(slog.NewTextHandler(io.Discard, nil))

	h := handler.NewAuthHandler(uc, log)
	jwtMW := middleware.NewJWTMiddleware(testTokens, log)

	engine := gin.New()
	engine.Use(middleware.RequestID())
	h.RegisterPublic(engine)

	protected := engine.Group("")
	protected.Use(jwtMW.Authenticate())
	h.RegisterProtected(protected)

	return engine
}

// post is a small JSON POST helper.
func post(t *testing.T, engine *gin.Engine, path, body string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)
	return w
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

// envelope decodes the APIResponse envelope for inspection.
func envelope(t *testing.T, body []byte) response.APIResponse {
	t.Helper()
	var env response.APIResponse
	require.NoError(t, json.Unmarshal(body, &env))
	return env
}

// -----------------------------------------------------------------------------
// /auth/signup
// -----------------------------------------------------------------------------

func TestSignup_Happy(t *testing.T) {
	uc := &fakeUseCase{
		registerOut: usecase.RegisterOutput{
			UserID:    "uuid-123",
			Email:     "a@b.com",
			CreatedAt: time.Now().UTC().Format(time.RFC3339),
		},
	}
	engine := newTestServer(uc)

	w := post(t, engine, "/auth/signup",
		`{"email":"a@b.com","password":"correct horse battery"}`)

	require.Equal(t, http.StatusCreated, w.Code)
	env := envelope(t, w.Body.Bytes())
	assert.Equal(t, http.StatusCreated, env.Code)
	assert.Equal(t, "uuid-123", env.Data.(map[string]any)["user_id"])
	assert.Equal(t, "a@b.com", env.Data.(map[string]any)["email"])

	// Critical security assertion: no password / hash anywhere in the response.
	body := strings.ToLower(string(w.Body.Bytes()))
	assert.NotContains(t, body, "password")
	assert.NotContains(t, body, "hash")
}

func TestSignup_DuplicateEmail_409(t *testing.T) {
	uc := &fakeUseCase{registerErr: entity.ErrEmailAlreadyExists}
	engine := newTestServer(uc)

	w := post(t, engine, "/auth/signup",
		`{"email":"a@b.com","password":"correct horse battery"}`)

	require.Equal(t, http.StatusConflict, w.Code)
	env := envelope(t, w.Body.Bytes())
	assert.Equal(t, "email already registered", env.Message)
}

func TestSignup_InvalidJSON_400(t *testing.T) {
	uc := &fakeUseCase{}
	engine := newTestServer(uc)

	w := post(t, engine, "/auth/signup", `{"email":"not-an-email"}`)

	require.Equal(t, http.StatusBadRequest, w.Code)
}

func TestSignup_InternalError_500(t *testing.T) {
	uc := &fakeUseCase{registerErr: errors.New("boom")}
	engine := newTestServer(uc)

	w := post(t, engine, "/auth/signup",
		`{"email":"a@b.com","password":"correct horse battery"}`)

	require.Equal(t, http.StatusInternalServerError, w.Code)
	env := envelope(t, w.Body.Bytes())
	assert.Equal(t, "internal error", env.Message)
}

// -----------------------------------------------------------------------------
// /auth/login
// -----------------------------------------------------------------------------

func TestLogin_Happy(t *testing.T) {
	uc := &fakeUseCase{
		loginOut: usecase.LoginOutput{
			Token:     "jwt-token-here",
			TokenType: "Bearer",
			ExpiresIn: 86400,
			UserID:    "uuid-123",
			Email:     "a@b.com",
		},
	}
	engine := newTestServer(uc)

	w := post(t, engine, "/auth/login",
		`{"email":"a@b.com","password":"correct horse battery"}`)

	require.Equal(t, http.StatusOK, w.Code)
	env := envelope(t, w.Body.Bytes())
	assert.Equal(t, "Bearer", env.Data.(map[string]any)["token_type"])
	assert.Equal(t, "jwt-token-here", env.Data.(map[string]any)["token"])
}

func TestLogin_InvalidCredentials_401(t *testing.T) {
	uc := &fakeUseCase{loginErr: entity.ErrInvalidCredentials}
	engine := newTestServer(uc)

	w := post(t, engine, "/auth/login",
		`{"email":"a@b.com","password":"wrong"}`)

	require.Equal(t, http.StatusUnauthorized, w.Code)
	env := envelope(t, w.Body.Bytes())
	assert.Equal(t, "invalid credentials", env.Message)
}

// -----------------------------------------------------------------------------
// /auth/me
// -----------------------------------------------------------------------------

func TestMe_WithValidToken_200(t *testing.T) {
	// Issue a token using the test TokenManager.
	token, err := testTokens.GenerateAccessToken("uuid-123", "a@b.com")
	require.NoError(t, err)

	uc := &fakeUseCase{
		meOut: usecase.MeOutput{
			UserID:    "uuid-123",
			Email:     "a@b.com",
			CreatedAt: time.Now().UTC().Format(time.RFC3339),
		},
	}
	engine := newTestServer(uc)

	w := get(t, engine, "/auth/me", token)

	require.Equal(t, http.StatusOK, w.Code)
	env := envelope(t, w.Body.Bytes())
	assert.Equal(t, "uuid-123", env.Data.(map[string]any)["user_id"])
	assert.Equal(t, "uuid-123", uc.lastMeUserID, "user_id from JWT should be passed to usecase")
}

func TestMe_NoToken_401(t *testing.T) {
	uc := &fakeUseCase{}
	engine := newTestServer(uc)

	w := get(t, engine, "/auth/me", "")

	require.Equal(t, http.StatusUnauthorized, w.Code)
}

func TestMe_ExpiredToken_401(t *testing.T) {
	// Manually craft an expired token signed with the test secret.
	expired := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{
		"iss":   "mobturret-server",
		"aud":   "mobturret-mobile",
		"sub":   "uuid-123",
		"email": "a@b.com",
		"iat":   time.Now().Add(-2 * time.Hour).Unix(),
		"exp":   time.Now().Add(-1 * time.Hour).Unix(),
	})
	token, err := expired.SignedString([]byte("test-secret-key-must-be-32-bytes-long-okay"))
	require.NoError(t, err)

	uc := &fakeUseCase{}
	engine := newTestServer(uc)

	w := get(t, engine, "/auth/me", token)

	require.Equal(t, http.StatusUnauthorized, w.Code)
}

func TestMe_WrongAlgorithm_401(t *testing.T) {
	// Token signed with a different algorithm (none / no sig) — should be rejected.
	claims := jwt.MapClaims{
		"iss":   "mobturret-server",
		"aud":   "mobturret-mobile",
		"sub":   "uuid-123",
		"email": "a@b.com",
		"iat":   time.Now().Unix(),
		"exp":   time.Now().Add(time.Hour).Unix(),
	}
	tok := jwt.NewWithClaims(jwt.SigningMethodNone, claims)
	token, err := tok.SignedString(jwt.UnsafeAllowNoneSignatureType)
	require.NoError(t, err)

	uc := &fakeUseCase{}
	engine := newTestServer(uc)

	w := get(t, engine, "/auth/me", token)

	require.Equal(t, http.StatusUnauthorized, w.Code,
		"alg=none token must be rejected with 401")
}

// -----------------------------------------------------------------------------
// RequestID middleware
// -----------------------------------------------------------------------------

func TestRequestID_EchoedInResponse(t *testing.T) {
	uc := &fakeUseCase{}
	engine := newTestServer(uc)

	req := httptest.NewRequest(http.MethodPost, "/auth/login",
		bytes.NewBufferString(`{"email":"a@b.com","password":"x"}`))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Request-Id", "caller-supplied-id-123")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)

	assert.Equal(t, "caller-supplied-id-123", w.Header().Get("X-Request-Id"),
		"caller-supplied X-Request-Id must be echoed in response")
}

func TestRequestID_GeneratedIfAbsent(t *testing.T) {
	uc := &fakeUseCase{}
	engine := newTestServer(uc)

	req := httptest.NewRequest(http.MethodPost, "/auth/login",
		bytes.NewBufferString(`{"email":"a@b.com","password":"x"}`))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)

	got := w.Header().Get("X-Request-Id")
	assert.NotEmpty(t, got, "server should generate a request_id when caller doesn't supply one")
}
