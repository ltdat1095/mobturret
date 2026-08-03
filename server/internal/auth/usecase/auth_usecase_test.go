package usecase_test

import (
	"context"
	"errors"
	"log/slog"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"mobturret/server/internal/auth/domain/entity"
	"mobturret/server/internal/auth/domain/repository"
	"mobturret/server/internal/auth/usecase"
	"mobturret/server/internal/platform/security"
)

// fakeUserRepo is an in-memory UserRepository for unit tests. It's
// not safe for concurrent use — fine for single-threaded tests.
type fakeUserRepo struct {
	users     map[string]entity.User // keyed by user_id
	byEmail   map[string]string      // email_key -> user_id
	createErr error                  // injected failure for Create
}

func newFakeUserRepo() *fakeUserRepo {
	return &fakeUserRepo{
		users:   make(map[string]entity.User),
		byEmail: make(map[string]string),
	}
}

func (f *fakeUserRepo) Create(_ context.Context, user entity.User) error {
	if f.createErr != nil {
		return f.createErr
	}
	if _, exists := f.byEmail[user.EmailKey]; exists {
		return entity.ErrEmailAlreadyExists
	}
	f.users[user.UserID] = user
	f.byEmail[user.EmailKey] = user.UserID
	return nil
}

func (f *fakeUserRepo) GetByEmail(_ context.Context, emailKey string) (entity.User, error) {
	id, ok := f.byEmail[emailKey]
	if !ok {
		return entity.User{}, entity.ErrUserNotFound
	}
	return f.users[id], nil
}

func (f *fakeUserRepo) GetByID(_ context.Context, userID string) (entity.User, error) {
	u, ok := f.users[userID]
	if !ok {
		return entity.User{}, entity.ErrUserNotFound
	}
	return u, nil
}

func newTestUC(t *testing.T, repo repository.UserRepository) *usecase.AuthUseCase {
	t.Helper()
	tokens := security.New(
		[]byte("test-secret-key-must-be-32-bytes-long-okay"),
		"mobturret-server",
		"mobturret-mobile",
		24,
	)
	log := slog.New(slog.NewTextHandler(testWriter{t: t}, &slog.HandlerOptions{Level: slog.LevelDebug}))
	return usecase.NewAuthUseCase(repo, tokens, log)
}

// testWriter pipes slog output to t.Log so test runs show structured logs.
type testWriter struct{ t *testing.T }

func (w testWriter) Write(p []byte) (int, error) {
	w.t.Log(strings.TrimRight(string(p), "\n"))
	return len(p), nil
}

// -----------------------------------------------------------------------------
// Register
// -----------------------------------------------------------------------------

func TestRegister_Happy(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	out, err := uc.Register(context.Background(), usecase.RegisterInput{
		Email:    "Alice@Example.com",
		Password: "correct horse battery",
	})
	require.NoError(t, err)
	assert.NotEmpty(t, out.UserID, "UserID should be a UUID")
	assert.Equal(t, "Alice@Example.com", out.Email, "display email preserved verbatim")
	assert.NotEmpty(t, out.CreatedAt)
	_, err = time.Parse(time.RFC3339, out.CreatedAt)
	assert.NoError(t, err, "CreatedAt should be RFC3339")
}

func TestRegister_DuplicateEmail(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	in := usecase.RegisterInput{Email: "a@b.com", Password: "correct horse battery"}
	_, err := uc.Register(context.Background(), in)
	require.NoError(t, err)

	_, err = uc.Register(context.Background(), in)
	require.Error(t, err)
	assert.True(t, errors.Is(err, entity.ErrEmailAlreadyExists),
		"duplicate email should yield ErrEmailAlreadyExists, got %v", err)
}

func TestRegister_PasswordTooLong(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	longPassword := strings.Repeat("a", 73) // bcrypt truncates at 72
	_, err := uc.Register(context.Background(), usecase.RegisterInput{
		Email:    "a@b.com",
		Password: longPassword,
	})
	require.Error(t, err)
	assert.True(t, errors.Is(err, entity.ErrInvalidInput))
}

func TestRegister_EmailNormalized(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	_, err := uc.Register(context.Background(), usecase.RegisterInput{
		Email: "  Alice@Example.COM ",
		Password: "correct horse battery",
	})
	require.NoError(t, err)

	// Second registration with same email (different casing) should fail.
	_, err = uc.Register(context.Background(), usecase.RegisterInput{
		Email: "alice@example.com",
		Password: "another password",
	})
	require.Error(t, err)
	assert.True(t, errors.Is(err, entity.ErrEmailAlreadyExists),
		"case-insensitive uniqueness check failed: got %v", err)
}

// -----------------------------------------------------------------------------
// Login
// -----------------------------------------------------------------------------

func TestLogin_Happy(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	reg, err := uc.Register(context.Background(), usecase.RegisterInput{
		Email:    "a@b.com",
		Password: "correct horse battery",
	})
	require.NoError(t, err)

	out, err := uc.Login(context.Background(), usecase.LoginInput{
		Email:    "a@b.com",
		Password: "correct horse battery",
	})
	require.NoError(t, err)
	assert.NotEmpty(t, out.Token)
	assert.Equal(t, "Bearer", out.TokenType)
	assert.Equal(t, 86400, out.ExpiresIn)
	assert.Equal(t, reg.UserID, out.UserID)
}

func TestLogin_WrongPassword(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	_, err := uc.Register(context.Background(), usecase.RegisterInput{
		Email:    "a@b.com",
		Password: "correct horse battery",
	})
	require.NoError(t, err)

	_, err = uc.Login(context.Background(), usecase.LoginInput{
		Email:    "a@b.com",
		Password: "wrong password",
	})
	require.Error(t, err)
	assert.True(t, errors.Is(err, entity.ErrInvalidCredentials))
}

func TestLogin_UnknownEmail_NoLeak(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	_, err := uc.Login(context.Background(), usecase.LoginInput{
		Email:    "nobody@nowhere.com",
		Password: "anything",
	})
	require.Error(t, err)
	// Critical: must NOT leak ErrUserNotFound — that would tell an
	// attacker the email isn't registered.
	assert.True(t, errors.Is(err, entity.ErrInvalidCredentials),
		"unknown email must return ErrInvalidCredentials (not ErrUserNotFound), got %v", err)
}

// -----------------------------------------------------------------------------
// Me
// -----------------------------------------------------------------------------

func TestMe_Happy(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	reg, err := uc.Register(context.Background(), usecase.RegisterInput{
		Email:    "a@b.com",
		Password: "correct horse battery",
	})
	require.NoError(t, err)

	out, err := uc.Me(context.Background(), reg.UserID)
	require.NoError(t, err)
	assert.Equal(t, reg.UserID, out.UserID)
	assert.Equal(t, "a@b.com", out.Email)
	assert.Equal(t, reg.CreatedAt, out.CreatedAt)
}

func TestMe_UnknownUser(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	_, err := uc.Me(context.Background(), "ghost-user-id")
	require.Error(t, err)
	assert.True(t, errors.Is(err, entity.ErrUserNotFound))
}

// -----------------------------------------------------------------------------
// password_hash leak scan
// -----------------------------------------------------------------------------

func TestRegisterOutput_NoPasswordHash(t *testing.T) {
	repo := newFakeUserRepo()
	uc := newTestUC(t, repo)

	out, err := uc.Register(context.Background(), usecase.RegisterInput{
		Email:    "a@b.com",
		Password: "correct horse battery",
	})
	require.NoError(t, err)

	// The output struct literally has no PasswordHash field; this is
	// a compile-time guarantee. The dynamic check below is belt-and-braces.
	json, _ := jsonMarshal(out)
	assert.NotContains(t, strings.ToLower(string(json)), "hash")
	assert.NotContains(t, strings.ToLower(string(json)), "password")
}
