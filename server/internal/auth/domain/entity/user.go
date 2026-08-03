// Package entity defines the auth domain's core data types and
// sentinel errors. Domain layer has no outbound dependencies.
//
// The User struct doubles as the DynamoDB row (via dynamodbav tags)
// and the wire DTO (via json tags). Sensitive fields are json:"-"
// so they never leak to API responses.
package entity

import "errors"

// User is a registered MobTurret account.
//
// Wire-format rules:
//   - PasswordHash is json:"-" — never serialized to clients
//   - EmailKey is json:"-" — internal uniqueness key, derived from Email
//   - JWTIssuedAt is json:"-" — internal field, not part of /me response
//   - FCMToken is json:"-" — push-notification handle, not yet exposed
type User struct {
	UserID       string `dynamodbav:"user_id"       json:"-"`
	Email        string `dynamodbav:"email"         json:"email"`
	EmailKey     string `dynamodbav:"email_key"     json:"-"`
	PasswordHash string `dynamodbav:"password_hash" json:"-"`
	CreatedAt    string `dynamodbav:"created_at"    json:"created_at"`
	JWTIssuedAt  string `dynamodbav:"jwt_issued_at" json:"-"`
	FCMToken     string `dynamodbav:"fcm_token,omitempty" json:"-"`
}

// Domain sentinel errors. Use errors.Is to detect at any layer.
//
// Reference bug fixed: the reference codebase calls
// errors.Is(err, errors.New("email already exists")) which NEVER
// matches because errors.New returns a fresh value each call. We use
// proper sentinels declared here once.
var (
	ErrEmailAlreadyExists = errors.New("email already registered")
	ErrUserNotFound       = errors.New("user not found")
	ErrInvalidCredentials = errors.New("invalid credentials")
	ErrInvalidInput       = errors.New("invalid input")
)
