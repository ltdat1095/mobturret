package repository_test

import (
	"context"
	"errors"
	"os"
	"testing"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/dynamodb"
	ddbtypes "github.com/aws/aws-sdk-go-v2/service/dynamodb/types"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"mobturret/server/internal/auth/adapter/repository"
	"mobturret/server/internal/auth/domain/entity"
)

// These tests run against DynamoDB Local. The endpoint defaults to
// http://localhost:8181 (the docker-compose service), so a plain
// `go test ./...` exercises them whenever the container is up. They
// skip — loudly, naming the reason — only when the endpoint is
// unreachable, e.g. in CI without Docker. Override with:
//
//	DDB_TEST_ENDPOINT=http://host:port
//
// Set DDB_TEST_ENDPOINT=off to force a skip.
const (
	envEndpoint = "DDB_TEST_ENDPOINT"
	envRegion   = "DDB_TEST_REGION"
	envKeyID    = "DDB_TEST_KEY_ID"
	envSecret   = "DDB_TEST_SECRET"

	defaultTestEndpoint = "http://localhost:8181"
)

// ddbTestClient returns a *dynamodb.Client pointed at the test
// DynamoDB Local, or t.Skip if it's unreachable. The returned
// teardown drops the test table.
func ddbTestClient(t *testing.T) (*dynamodb.Client, string, func()) {
	t.Helper()

	endpoint := os.Getenv(envEndpoint)
	if endpoint == "" {
		endpoint = defaultTestEndpoint
	}
	if endpoint == "off" {
		t.Skipf("%s=off — skipping DDB integration test", envEndpoint)
	}
	region := os.Getenv(envRegion)
	if region == "" {
		region = "ap-southeast-1"
	}
	keyID := os.Getenv(envKeyID)
	if keyID == "" {
		keyID = "dummy"
	}
	secret := os.Getenv(envSecret)
	if secret == "" {
		secret = "dummy"
	}

	cfg := aws.Config{
		Region:       region,
		Credentials:  credentials.NewStaticCredentialsProvider(keyID, secret, ""),
		BaseEndpoint: aws.String(endpoint),
	}
	client := dynamodb.NewFromConfig(cfg)

	// Probe reachability before doing real work, so an absent
	// container yields a clean skip instead of a confusing failure.
	probeCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if _, err := client.ListTables(probeCtx, &dynamodb.ListTablesInput{}); err != nil {
		t.Skipf("DynamoDB Local unreachable at %s (%v) — run `make ddb-up`", endpoint, err)
	}

	// Per-test unique table name so parallel test runs don't collide.
	tableName := "Users-test-" + sanitize(t.Name())

	ctx := context.Background()
	// A previous crashed run may have left the table behind.
	_, _ = client.DeleteTable(ctx, &dynamodb.DeleteTableInput{TableName: &tableName})

	_, err := client.CreateTable(ctx, &dynamodb.CreateTableInput{
		TableName: &tableName,
		AttributeDefinitions: []ddbtypes.AttributeDefinition{
			{AttributeName: aws.String("user_id"), AttributeType: ddbtypes.ScalarAttributeTypeS},
		},
		KeySchema: []ddbtypes.KeySchemaElement{
			{AttributeName: aws.String("user_id"), KeyType: ddbtypes.KeyTypeHash},
		},
		BillingMode: ddbtypes.BillingModePayPerRequest,
	})
	require.NoError(t, err, "create test table")

	teardown := func() {
		_, _ = client.DeleteTable(context.Background(), &dynamodb.DeleteTableInput{
			TableName: &tableName,
		})
	}

	return client, tableName, teardown
}

// sanitize strips characters that DynamoDB allows (alphanumerics,
// underscore, dash, dot) but keeps it short for readability.
func sanitize(s string) string {
	out := make([]byte, 0, len(s))
	for i := 0; i < len(s) && len(out) < 40; i++ {
		c := s[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
			out = append(out, c)
		default:
			out = append(out, '_')
		}
	}
	return string(out)
}

// -----------------------------------------------------------------------------
// Create
// -----------------------------------------------------------------------------

func TestCreate_Happy(t *testing.T) {
	client, tableName, teardown := ddbTestClient(t)
	defer teardown()

	repo := repository.NewDynamoUserRepo(client, tableName)

	user := entity.User{
		UserID:       "user-1",
		Email:        "Alice@Example.com",
		EmailKey:     "alice@example.com",
		PasswordHash: "hashed",
		CreatedAt:    "2026-07-31T00:00:00Z",
	}

	require.NoError(t, repo.Create(context.Background(), user))

	got, err := repo.GetByID(context.Background(), "user-1")
	require.NoError(t, err)
	assert.Equal(t, "Alice@Example.com", got.Email)
	assert.Equal(t, "alice@example.com", got.EmailKey)
	assert.Equal(t, "hashed", got.PasswordHash)
}

func TestCreate_DuplicateEmail_ReturnsSentinel(t *testing.T) {
	client, tableName, teardown := ddbTestClient(t)
	defer teardown()

	repo := repository.NewDynamoUserRepo(client, tableName)

	first := entity.User{
		UserID:       "user-1",
		Email:        "a@b.com",
		EmailKey:     "a@b.com",
		PasswordHash: "h1",
		CreatedAt:    "2026-07-31T00:00:00Z",
	}
	require.NoError(t, repo.Create(context.Background(), first))

	second := entity.User{
		UserID:       "user-2", // different user_id
		Email:        "a@b.com",
		EmailKey:     "a@b.com",
		PasswordHash: "h2",
		CreatedAt:    "2026-07-31T00:00:01Z",
	}
	err := repo.Create(context.Background(), second)
	require.Error(t, err)
	assert.True(t, errors.Is(err, entity.ErrEmailAlreadyExists),
		"second user with same email must yield ErrEmailAlreadyExists, got %v", err)
}

// -----------------------------------------------------------------------------
// GetByEmail
// -----------------------------------------------------------------------------

func TestGetByEmail_Happy(t *testing.T) {
	client, tableName, teardown := ddbTestClient(t)
	defer teardown()

	repo := repository.NewDynamoUserRepo(client, tableName)

	require.NoError(t, repo.Create(context.Background(), entity.User{
		UserID:       "user-1",
		Email:        "a@b.com",
		EmailKey:     "a@b.com",
		PasswordHash: "h",
		CreatedAt:    "2026-07-31T00:00:00Z",
	}))

	got, err := repo.GetByEmail(context.Background(), "a@b.com")
	require.NoError(t, err)
	assert.Equal(t, "user-1", got.UserID)
	assert.Equal(t, "a@b.com", got.Email)
	// Login compares this hash. If GetByEmail resolves to the email
	// sentinel row instead of the user row, this comes back empty and
	// every login fails with "invalid credentials".
	assert.Equal(t, "h", got.PasswordHash,
		"GetByEmail must return the user row, not the email sentinel")
}

func TestGetByEmail_Unknown_ReturnsSentinel(t *testing.T) {
	client, tableName, teardown := ddbTestClient(t)
	defer teardown()

	repo := repository.NewDynamoUserRepo(client, tableName)

	_, err := repo.GetByEmail(context.Background(), "nobody@nowhere.com")
	require.Error(t, err)
	assert.True(t, errors.Is(err, entity.ErrUserNotFound))
}

// -----------------------------------------------------------------------------
// GetByID
// -----------------------------------------------------------------------------

func TestGetByID_Unknown_ReturnsSentinel(t *testing.T) {
	client, tableName, teardown := ddbTestClient(t)
	defer teardown()

	repo := repository.NewDynamoUserRepo(client, tableName)

	_, err := repo.GetByID(context.Background(), "ghost-id")
	require.Error(t, err)
	assert.True(t, errors.Is(err, entity.ErrUserNotFound))
}
