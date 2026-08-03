// Package repository defines the persistence interface for users.
// Concrete implementations live under adapter/repository (DynamoDB
// today; could be SQL/memory in tests).
//
// Interface-segregation principle: only methods the usecase layer
// actually needs. Don't add speculative queries.
package repository

import (
	"context"

	"mobturret/server/internal/auth/domain/entity"
)

// UserRepository is the persistence boundary for User aggregates.
type UserRepository interface {
	// Create inserts a new user. Must be atomic — either both the
	// user row and the email-sentinel row are written, or neither
	// is. Returns ErrEmailAlreadyExists if the email is taken.
	Create(ctx context.Context, user entity.User) error

	// GetByEmail looks up the user by the lowercased+trimmed email
	// key. Returns ErrUserNotFound when no such user exists.
	GetByEmail(ctx context.Context, emailKey string) (entity.User, error)

	// GetByID looks up the user by user_id. Returns ErrUserNotFound
	// when no such user exists.
	GetByID(ctx context.Context, userID string) (entity.User, error)
}
