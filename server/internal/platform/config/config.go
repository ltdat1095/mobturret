// Package config loads server configuration from .env files and
// environment variables via Viper.
//
// Source precedence (highest first):
//  1. process env vars (viper.AutomaticEnv)
//  2. .env file (if found)
//  3. defaults set in setDefaults
//
// Config is the single source of truth for runtime tunables. All other
// packages read Config (or a sub-struct) via Wire, never viper directly.
//
// Why we build Config with explicit getters instead of viper.Unmarshal:
// .env files are flat — Viper's "env" parser produces keys like
// "jwt_secret_key", not the nested "jwt.secretkey" that Unmarshal
// derives from the Go struct shape. Worse, AutomaticEnv is invisible
// to Unmarshal, so env-var-only deployments (Lambda) would silently
// get zero values. Reading each key by name sidesteps both problems
// and keeps one code path for files and env vars alike.
package config

import (
	"fmt"
	"strings"

	"github.com/spf13/viper"
)

// Config is the top-level config struct. Sub-structs group related
// settings so handlers/usecases can take just what they need.
type Config struct {
	Server ServerConfig
	Log    LogConfig
	AWS    AWSConfig
	DDB    DynamoDBConfig
	JWT    JWTConfig
}

type ServerConfig struct {
	Port string
	Env  string // development | production
}

type LogConfig struct {
	Format string // console | json
	Level  string // debug | info | warn | error
}

type AWSConfig struct {
	Region          string
	AccessKeyID     string
	SecretAccessKey string
}

type DynamoDBConfig struct {
	Endpoint   string // empty → real AWS; set → DynamoDB Local
	UsersTable string
}

type JWTConfig struct {
	SecretKey string
	Issuer    string
	Audience  string
	TTLHours  int
}

// LoadConfig reads from .env (if present) then env vars. configPath
// is the explicit path to a .env file; if empty, viper searches the
// CWD and parents.
func LoadConfig(configPath string) (*Config, error) {
	v := viper.New()

	setDefaults(v)
	bindEnvVars(v)

	if configPath != "" {
		v.SetConfigFile(configPath)
	} else {
		v.SetConfigName(".env")
		v.SetConfigType("env")
		v.AddConfigPath(".")
		v.AddConfigPath("./..")
		v.AddConfigPath("./../..")
	}

	// .env is optional — fall through to defaults if missing.
	if err := v.ReadInConfig(); err != nil {
		if _, ok := err.(viper.ConfigFileNotFoundError); !ok {
			return nil, fmt.Errorf("read config: %w", err)
		}
	}

	cfg := buildConfig(v)
	if err := cfg.validate(); err != nil {
		return nil, fmt.Errorf("invalid config: %w", err)
	}

	return &cfg, nil
}

// LoadConfigFromEnv skips the .env lookup and reads everything from
// process env vars. Used by Lambda entry points where there is no
// filesystem.
func LoadConfigFromEnv() (*Config, error) {
	v := viper.New()
	setDefaults(v)
	bindEnvVars(v)

	cfg := buildConfig(v)
	if err := cfg.validate(); err != nil {
		return nil, fmt.Errorf("invalid config: %w", err)
	}
	return &cfg, nil
}

// buildConfig maps viper keys onto the Config struct. Keys are the
// lowercased .env / env-var names, so a single lookup resolves either
// source (AutomaticEnv makes "jwt_secret_key" read JWT_SECRET_KEY).
func buildConfig(v *viper.Viper) Config {
	return Config{
		Server: ServerConfig{
			Port: v.GetString("port"),
			Env:  v.GetString("env"),
		},
		Log: LogConfig{
			Format: v.GetString("log_format"),
			Level:  v.GetString("log_level"),
		},
		AWS: AWSConfig{
			Region:          v.GetString("aws_region"),
			AccessKeyID:     v.GetString("aws_access_key_id"),
			SecretAccessKey: v.GetString("aws_secret_access_key"),
		},
		DDB: DynamoDBConfig{
			Endpoint:   v.GetString("dynamodb_endpoint"),
			UsersTable: v.GetString("users_table"),
		},
		JWT: JWTConfig{
			SecretKey: v.GetString("jwt_secret_key"),
			Issuer:    v.GetString("jwt_issuer"),
			Audience:  v.GetString("jwt_audience"),
			TTLHours:  v.GetInt("jwt_ttl_hours"),
		},
	}
}

func bindEnvVars(v *viper.Viper) {
	v.SetEnvKeyReplacer(strings.NewReplacer(".", "_"))
	v.AutomaticEnv()
}

func setDefaults(v *viper.Viper) {
	v.SetDefault("port", "8080")
	v.SetDefault("env", "development")
	v.SetDefault("log_format", "console")
	v.SetDefault("log_level", "info")
	v.SetDefault("aws_region", "ap-southeast-1")
	v.SetDefault("aws_access_key_id", "dummy")
	v.SetDefault("aws_secret_access_key", "dummy")
	v.SetDefault("dynamodb_endpoint", "http://localhost:8181")
	v.SetDefault("users_table", "Users")
	v.SetDefault("jwt_issuer", "mobturret-server")
	v.SetDefault("jwt_audience", "mobturret-mobile")
	v.SetDefault("jwt_ttl_hours", 24)
}

func (c *Config) validate() error {
	if len(c.JWT.SecretKey) < 32 {
		return fmt.Errorf("JWT_SECRET_KEY must be at least 32 bytes (got %d)", len(c.JWT.SecretKey))
	}
	if c.JWT.TTLHours <= 0 {
		return fmt.Errorf("JWT_TTL_HOURS must be > 0 (got %d)", c.JWT.TTLHours)
	}
	if c.Server.Port == "" {
		return fmt.Errorf("PORT is required")
	}
	if c.DDB.UsersTable == "" {
		return fmt.Errorf("USERS_TABLE is required")
	}
	return nil
}
