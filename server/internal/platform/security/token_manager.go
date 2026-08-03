// Package security handles JWT issuance and validation. Tokens are
// HS256 (the reference codebase uses HS256 too; we keep that decision).
// Claims are iss, aud, sub (user_id), email, iat, exp.
//
// TokenManager is the single place that signs / verifies. Middleware
// calls ValidateToken (no duplication of parsing logic, unlike the
// reference codebase).
package security

import (
	"errors"
	"fmt"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

// ErrInvalidToken wraps any validation failure (parse, signature,
// expiry, algorithm mismatch). Callers should map this to 401.
var ErrInvalidToken = errors.New("invalid token")

// Claims is the JWT payload.
type Claims struct {
	UserID string `json:"sub"`
	Email  string `json:"email"`
	jwt.RegisteredClaims
}

// TokenManager issues and verifies JWTs.
type TokenManager struct {
	secret   []byte
	issuer   string
	audience string
	ttl      time.Duration
}

// New constructs a TokenManager. secret must be ≥ 32 bytes (HS256 RFC
// 7518 requirement).
func New(secret []byte, issuer, audience string, ttlHours int) *TokenManager {
	return &TokenManager{
		secret:   secret,
		issuer:   issuer,
		audience: audience,
		ttl:      time.Duration(ttlHours) * time.Hour,
	}
}

// GenerateAccessToken signs an HS256 token for the given user.
func (t *TokenManager) GenerateAccessToken(userID, email string) (string, error) {
	now := time.Now().UTC()
	claims := Claims{
		UserID: userID,
		Email:  email,
		RegisteredClaims: jwt.RegisteredClaims{
			Issuer:    t.issuer,
			Audience:  jwt.ClaimStrings{t.audience},
			Subject:   userID,
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(t.ttl)),
			NotBefore: jwt.NewNumericDate(now),
		},
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	signed, err := token.SignedString(t.secret)
	if err != nil {
		return "", fmt.Errorf("sign token: %w", err)
	}
	return signed, nil
}

// ValidateToken parses and verifies a token. Returns ErrInvalidToken
// for any failure mode (parse, signature, expiry, algorithm). The
// parsed claims are returned on success.
//
// HS256-only: any other alg (e.g. "none", RS256) is rejected with
// ErrInvalidToken to prevent the classic alg-confusion attack.
func (t *TokenManager) ValidateToken(tokenStr string) (*Claims, error) {
	parser := jwt.NewParser(
		jwt.WithValidMethods([]string{"HS256"}),
		jwt.WithIssuer(t.issuer),
		jwt.WithAudience(t.audience),
	)

	claims := &Claims{}
	_, err := parser.ParseWithClaims(tokenStr, claims, func(tok *jwt.Token) (any, error) {
		return t.secret, nil
	})
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalidToken, err)
	}
	return claims, nil
}

// TTL returns the configured token lifetime (useful for the /login
// response's expires_in field).
func (t *TokenManager) TTL() time.Duration {
	return t.ttl
}
