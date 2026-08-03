// Package usecase holds the auth UseCase interface, its concrete
// implementation, and the input/output DTOs that the HTTP layer
// binds requests to.
//
// The DTOs use Gin's binding tags for declarative validation. The
// handler calls c.ShouldBindJSON(&input) — any tag failure becomes
// a 400 with the binding error in details.
package usecase

// RegisterInput is the JSON body for POST /auth/signup.
type RegisterInput struct {
	Email    string `json:"email"    binding:"required,email"`
	Password string `json:"password" binding:"required,min=8,max=72"`
}

// RegisterOutput is the response for a successful signup.
type RegisterOutput struct {
	UserID    string `json:"user_id"`
	Email     string `json:"email"`
	CreatedAt string `json:"created_at"`
}

// LoginInput is the JSON body for POST /auth/login.
type LoginInput struct {
	Email    string `json:"email"    binding:"required,email"`
	Password string `json:"password" binding:"required"`
}

// LoginOutput is the response for a successful login. Token is the
// JWT; expires_in is in seconds.
type LoginOutput struct {
	Token     string `json:"token"`
	TokenType string `json:"token_type"`
	ExpiresIn int    `json:"expires_in"`
	UserID    string `json:"user_id"`
	Email     string `json:"email"`
}

// MeOutput is the response for GET /auth/me.
type MeOutput struct {
	UserID    string `json:"user_id"`
	Email     string `json:"email"`
	CreatedAt string `json:"created_at"`
}
