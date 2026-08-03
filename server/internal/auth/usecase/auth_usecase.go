package usecase

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/google/uuid"
	"golang.org/x/crypto/bcrypt"

	"mobturret/server/internal/auth/domain/entity"
	"mobturret/server/internal/auth/domain/repository"
	"mobturret/server/internal/platform/logger"
	"mobturret/server/internal/platform/security"
)

// AuthUseCaseInterface is the contract the HTTP layer depends on.
// Handlers must take this interface, not *AuthUseCase, so tests can
// substitute fakes.
type AuthUseCaseInterface interface {
	Register(ctx context.Context, in RegisterInput) (RegisterOutput, error)
	Login(ctx context.Context, in LoginInput) (LoginOutput, error)
	Me(ctx context.Context, userID string) (MeOutput, error)
}

// AuthUseCase is the concrete implementation. Depends only on
// interfaces (UserRepository, TokenManager) — no transport, no HTTP.
type AuthUseCase struct {
	repo   repository.UserRepository
	tokens *security.TokenManager
	logger *slog.Logger
}

// NewAuthUseCase wires the dependencies. Used by Wire's provider set.
func NewAuthUseCase(
	repo repository.UserRepository,
	tokens *security.TokenManager,
	logger *slog.Logger,
) *AuthUseCase {
	return &AuthUseCase{repo: repo, tokens: tokens, logger: logger}
}

// Register hashes the password and persists the user. Returns
// ErrEmailAlreadyExists if the email is taken.
func (uc *AuthUseCase) Register(ctx context.Context, in RegisterInput) (RegisterOutput, error) {
	log := logger.With(ctx, uc.logger)

	if len(in.Password) > 72 {
		// Bcrypt silently truncates > 72 bytes, which would let two
		// different passwords match. Reject explicitly.
		return RegisterOutput{}, fmt.Errorf("password too long: %w", entity.ErrInvalidInput)
	}

	hash, err := bcrypt.GenerateFromPassword([]byte(in.Password), bcrypt.DefaultCost)
	if err != nil {
		return RegisterOutput{}, fmt.Errorf("hash password: %w", err)
	}

	user := entity.User{
		UserID:       uuid.NewString(),
		Email:        in.Email,
		EmailKey:     normalizeEmail(in.Email),
		PasswordHash: string(hash),
		CreatedAt:    time.Now().UTC().Format(time.RFC3339),
	}

	if err := uc.repo.Create(ctx, user); err != nil {
		return RegisterOutput{}, err
	}

	log.InfoContext(ctx, "user registered",
		slog.String("user_id", user.UserID),
		slog.String("email_key", user.EmailKey),
	)

	return RegisterOutput{
		UserID:    user.UserID,
		Email:     user.Email,
		CreatedAt: user.CreatedAt,
	}, nil
}

// Login verifies credentials and issues a JWT. Returns
// ErrInvalidCredentials for any failure (unknown email OR wrong
// password) so we don't leak which.
func (uc *AuthUseCase) Login(ctx context.Context, in LoginInput) (LoginOutput, error) {
	log := logger.With(ctx, uc.logger)
	emailKey := normalizeEmail(in.Email)

	user, err := uc.repo.GetByEmail(ctx, emailKey)
	if err != nil {
		if errors.Is(err, entity.ErrUserNotFound) {
			// Don't leak that the email doesn't exist.
			return LoginOutput{}, entity.ErrInvalidCredentials
		}
		return LoginOutput{}, err
	}

	if err := bcrypt.CompareHashAndPassword([]byte(user.PasswordHash), []byte(in.Password)); err != nil {
		log.InfoContext(ctx, "login failed",
			slog.String("email_key", emailKey),
			slog.String("reason", "bad_password"),
		)
		return LoginOutput{}, entity.ErrInvalidCredentials
	}

	token, err := uc.tokens.GenerateAccessToken(user.UserID, user.Email)
	if err != nil {
		return LoginOutput{}, fmt.Errorf("issue token: %w", err)
	}

	log.InfoContext(ctx, "user logged in",
		slog.String("user_id", user.UserID),
	)

	return LoginOutput{
		Token:     token,
		TokenType: "Bearer",
		ExpiresIn: int(uc.tokens.TTL().Seconds()),
		UserID:    user.UserID,
		Email:     user.Email,
	}, nil
}

// Me returns the public profile for an authenticated user. The
// user_id comes from the JWT middleware (handlers.UserID(c)) — the
// usecase trusts the caller.
func (uc *AuthUseCase) Me(ctx context.Context, userID string) (MeOutput, error) {
	user, err := uc.repo.GetByID(ctx, userID)
	if err != nil {
		return MeOutput{}, err
	}
	return MeOutput{
		UserID:    user.UserID,
		Email:     user.Email,
		CreatedAt: user.CreatedAt,
	}, nil
}

// normalizeEmail produces the canonical key used for uniqueness
// lookups. Display value (in.Email) is preserved verbatim.
func normalizeEmail(email string) string {
	return strings.ToLower(strings.TrimSpace(email))
}

// Compile-time assertion that AuthUseCase satisfies the interface.
var _ AuthUseCaseInterface = (*AuthUseCase)(nil)
