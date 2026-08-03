// Package repository contains DynamoDB-backed implementations of the
// domain repository interfaces. Keep SDK types out of the domain
// layer; this adapter is the only place that touches them.
package repository

import (
	"context"
	"errors"
	"fmt"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/service/dynamodb"
	ddbtypes "github.com/aws/aws-sdk-go-v2/service/dynamodb/types"

	"mobturret/server/internal/auth/domain/entity"
	domainrepo "mobturret/server/internal/auth/domain/repository"
	"mobturret/server/internal/platform/database"
)

// DynamoUserRepo is the DynamoDB-backed UserRepository.
//
// Email uniqueness without a GSI: writes two items in one
// TransactWriteItems call, one keyed by user_id and one sentinel
// keyed by EMAIL#<email_key>. A second signup with the same email
// gets a ConditionalCheckFailed on the sentinel — atomic, race-free.
type DynamoUserRepo struct {
	client    *dynamodb.Client
	tableName string
}

// NewDynamoUserRepo is the Wire-friendly constructor.
func NewDynamoUserRepo(client *dynamodb.Client, tableName string) *DynamoUserRepo {
	return &DynamoUserRepo{client: client, tableName: tableName}
}

// Compile-time interface check.
var _ domainrepo.UserRepository = (*DynamoUserRepo)(nil)

// emailSentinelPrefix prefixes the email-sentinel row's PK.
const emailSentinelPrefix = "EMAIL#"

func emailSentinelKey(emailKey string) string {
	return emailSentinelPrefix + emailKey
}

// emailSentinel is the storage-only row that reserves an email_key and
// points at the user row that owns it. It deliberately is not
// entity.User: it holds no credentials, and it needs a
// target_user_id attribute that has no meaning in the domain.
type emailSentinel struct {
	UserID       string `dynamodbav:"user_id"` // "EMAIL#<email_key>"
	EmailKey     string `dynamodbav:"email_key"`
	TargetUserID string `dynamodbav:"target_user_id"`
	CreatedAt    string `dynamodbav:"created_at"`
}

// Create writes both the user row and the email-sentinel row in one
// atomic transaction. Returns ErrEmailAlreadyExists if the email is
// taken.
func (r *DynamoUserRepo) Create(ctx context.Context, user entity.User) error {
	userItem, err := database.MarshalRecord(user)
	if err != nil {
		return fmt.Errorf("marshal user: %w", err)
	}

	// Sentinel carries only the email_key plus a pointer to the owning
	// user row — never the password hash.
	sentinelItem, err := database.MarshalRecord(emailSentinel{
		UserID:       emailSentinelKey(user.EmailKey),
		EmailKey:     user.EmailKey,
		TargetUserID: user.UserID,
		CreatedAt:    user.CreatedAt,
	})
	if err != nil {
		return fmt.Errorf("marshal sentinel: %w", err)
	}

	_, err = r.client.TransactWriteItems(ctx, &dynamodb.TransactWriteItemsInput{
		TransactItems: []ddbtypes.TransactWriteItem{
			{
				Put: &ddbtypes.Put{
					TableName:           &r.tableName,
					Item:                userItem,
					ConditionExpression: aws.String("attribute_not_exists(user_id)"),
				},
			},
			{
				Put: &ddbtypes.Put{
					TableName:           &r.tableName,
					Item:                sentinelItem,
					ConditionExpression: aws.String("attribute_not_exists(user_id)"),
				},
			},
		},
	})
	if err != nil {
		// Detect the email-already-exists case: a ConditionalCheck
		// failed on either Put. We don't distinguish which one — both
		// mean "email taken" (sentinel) or "user_id collision"
		// (effectively impossible with UUIDs).
		var canceled *ddbtypes.TransactionCanceledException
		if errors.As(err, &canceled) {
			for _, reason := range canceled.CancellationReasons {
				if reason.Code != nil && *reason.Code == "ConditionalCheckFailed" {
					return entity.ErrEmailAlreadyExists
				}
			}
		}
		return fmt.Errorf("transact write: %w", err)
	}
	return nil
}

// GetByEmail looks up the email-sentinel, then fetches the real user
// row by the user_id the sentinel points at.
func (r *DynamoUserRepo) GetByEmail(ctx context.Context, emailKey string) (entity.User, error) {
	item, err := r.getItem(ctx, emailSentinelKey(emailKey))
	if err != nil {
		return entity.User{}, err
	}

	var sentinel emailSentinel
	if err := database.UnmarshalRecord(item, &sentinel); err != nil {
		return entity.User{}, fmt.Errorf("unmarshal sentinel: %w", err)
	}
	if sentinel.TargetUserID == "" {
		return entity.User{}, fmt.Errorf("sentinel %q has no target_user_id", sentinel.UserID)
	}

	// Sentinel exists but user row missing — shouldn't happen unless
	// someone manually deleted it. GetByID already maps that to
	// ErrUserNotFound.
	return r.GetByID(ctx, sentinel.TargetUserID)
}

// GetByID fetches the user row by user_id.
func (r *DynamoUserRepo) GetByID(ctx context.Context, userID string) (entity.User, error) {
	item, err := r.getItem(ctx, userID)
	if err != nil {
		return entity.User{}, err
	}

	var user entity.User
	if err := database.UnmarshalRecord(item, &user); err != nil {
		return entity.User{}, fmt.Errorf("unmarshal user: %w", err)
	}
	return user, nil
}

// getItem is the GetItem helper. Returns ErrUserNotFound when the row
// is absent.
func (r *DynamoUserRepo) getItem(ctx context.Context, userID string) (map[string]ddbtypes.AttributeValue, error) {
	out, err := r.client.GetItem(ctx, &dynamodb.GetItemInput{
		TableName: &r.tableName,
		Key: map[string]ddbtypes.AttributeValue{
			"user_id": &ddbtypes.AttributeValueMemberS{Value: userID},
		},
	})
	if err != nil {
		return nil, fmt.Errorf("get item: %w", err)
	}
	if out.Item == nil {
		return nil, entity.ErrUserNotFound
	}
	return out.Item, nil
}
