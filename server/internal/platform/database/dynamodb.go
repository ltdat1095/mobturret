// Package database wraps the DynamoDB client construction, table
// provisioning, and value (un)marshaling. Repositories in
// internal/<module>/adapter/repository/ use this package's helpers
// rather than touching the SDK types directly.
package database

import (
	"context"
	"errors"
	"fmt"

	"github.com/aws/aws-sdk-go-v2/aws"
	awsconfig "github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/feature/dynamodb/attributevalue"
	"github.com/aws/aws-sdk-go-v2/service/dynamodb"
	ddbtypes "github.com/aws/aws-sdk-go-v2/service/dynamodb/types"

	"mobturret/server/internal/platform/config"
)

// NewDynamoDBClient builds a DynamoDB client. If cfg.DDB.Endpoint is
// non-empty (DynamoDB Local), it pins credentials + endpoint; otherwise
// it uses the default config (env / instance / shared config) and the
// real AWS endpoint.
func NewDynamoDBClient(ctx context.Context, cfg *config.Config) (*dynamodb.Client, error) {
	if cfg.DDB.Endpoint != "" {
		return dynamodb.NewFromConfig(aws.Config{
			Region: cfg.AWS.Region,
			Credentials: credentials.NewStaticCredentialsProvider(
				cfg.AWS.AccessKeyID,
				cfg.AWS.SecretAccessKey,
				"",
			),
			BaseEndpoint: aws.String(cfg.DDB.Endpoint),
		}), nil
	}

	// Production path: load from default credential chain.
	awsCfg, err := awsconfig.LoadDefaultConfig(ctx, awsconfig.WithRegion(cfg.AWS.Region))
	if err != nil {
		return nil, fmt.Errorf("load default AWS config: %w", err)
	}
	return dynamodb.NewFromConfig(awsCfg), nil
}

// EnsureTablesExist creates any of the given tables that don't already
// exist. Idempotent: DescribeTable first, only CreateTable if the
// table is missing. Used at server startup in dev; Phase 2 swaps this
// for CloudFormation-managed tables.
func EnsureTablesExist(ctx context.Context, client *dynamodb.Client, tables []TableSpec) error {
	for _, t := range tables {
		exists, err := tableExists(ctx, client, t.Name)
		if err != nil {
			return fmt.Errorf("describe %s: %w", t.Name, err)
		}
		if exists {
			continue
		}
		if err := createTable(ctx, client, t); err != nil {
			return fmt.Errorf("create %s: %w", t.Name, err)
		}
	}
	return nil
}

func tableExists(ctx context.Context, client *dynamodb.Client, name string) (bool, error) {
	_, err := client.DescribeTable(ctx, &dynamodb.DescribeTableInput{
		TableName: &name,
	})
	if err == nil {
		return true, nil
	}
	var notFound *ddbtypes.ResourceNotFoundException
	if errors.As(err, &notFound) {
		return false, nil
	}
	return false, err
}

func createTable(ctx context.Context, client *dynamodb.Client, t TableSpec) error {
	input := &dynamodb.CreateTableInput{
		TableName: &t.Name,
		AttributeDefinitions: []ddbtypes.AttributeDefinition{
			{AttributeName: &t.PK, AttributeType: ddbtypes.ScalarAttributeTypeS},
		},
		KeySchema: []ddbtypes.KeySchemaElement{
			{AttributeName: &t.PK, KeyType: ddbtypes.KeyTypeHash},
		},
		BillingMode: ddbtypes.BillingModePayPerRequest,
	}
	_, err := client.CreateTable(ctx, input)
	return err
}

// TableSpec describes one DynamoDB table. Phase 1 only needs the PK;
// GSIs are added when needed (Phase 2 may introduce them).
type TableSpec struct {
	Name string
	PK   string
}

// UsersTable returns the standard Users table spec.
func UsersTable(name string) TableSpec {
	return TableSpec{Name: name, PK: "user_id"}
}

// MarshalRecord serializes any tagged struct into a DynamoDB item.
func MarshalRecord(v any) (map[string]ddbtypes.AttributeValue, error) {
	return attributevalue.MarshalMap(v)
}

// UnmarshalRecord deserializes a DynamoDB item into the destination.
func UnmarshalRecord(item map[string]ddbtypes.AttributeValue, dst any) error {
	return attributevalue.UnmarshalMap(item, dst)
}
