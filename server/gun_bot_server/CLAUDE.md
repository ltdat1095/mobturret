# gun_bot_server — Go Lambda handlers

**Status: planned.** This folder is a placeholder; nothing is built
yet.

All MobTurret Lambda handlers are written in Go, packaged as one
binary per handler, and deployed via the CloudFormation templates in
[`../aws/`](../aws/).

## Planned layout (single Go module)

```
server/gun_bot_server/
├── go.mod
├── go.sum
├── Makefile                      # build / test / vet / zip targets
├── cmd/
│   ├── auth/main.go              # → bin/auth.zip
│   ├── telemetry_ingest/main.go  # → bin/telemetry_ingest.zip
│   ├── alert_router/main.go      # → bin/alert_router.zip
│   ├── presign_upload/main.go    # → bin/presign_upload.zip
│   └── media_list/main.go        # → bin/media_list.zip
├── internal/
│   ├── auth/                     # JWT issue + verify, shared by auth + authorizer
│   ├── db/                       # DynamoDB client wrappers per table
│   ├── iot/                      # IoT Data plane helpers (publish)
│   ├── notify/                   # SNS publish + FCM/APNs payload shaping
│   ├── storage/                  # S3 pre-sign helpers (upload + download URLs)
│   ├── api/                      # API Gateway request/response shapes, errors
│   └── cfg/                      # env-var loading, runtime config struct
├── testdata/                     # JSON fixtures, golden responses
└── README.md
```

One `go.mod`, many `cmd/` entry points. Lambdas stay independently
deployable while sharing internal helpers — easier than per-handler
modules and avoids the Go workspaces dance for a small service.

## Handlers

| Handler | Trigger | Internal packages used | Notes |
|---|---|---|---|
| `auth` | API Gateway `POST /auth/login`, `POST /auth/refresh` | `auth`, `db` (Users), `cfg` | Issues JWTs; verifies bcrypt-hashed passwords |
| `telemetry_ingest` | IoT Rule (`turret/+/telemetry` → Lambda) | `db` (Turrets, UsageLogs) | Idempotent on `robot_id + window`; updates last-seen timestamp |
| `alert_router` | IoT Rule (`turret/+/evt/alert` → Lambda) | `db` (Turrets), `storage` (pre-sign), `notify` (SNS) | Reads `video_capture_enabled`; if true, hands the A55 a pre-signed PUT URL and posts to SNS |
| `presign_upload` | API Gateway `POST /media/presign-upload` | `storage`, `auth` (verify JWT + `robot_id` claim) | Returns pre-signed PUT URL (TTL 5 min — open question) |
| `media_list` | API Gateway `GET /media` | `db` (SavedMedia), `storage` (pre-sign GET), `auth` | Returns paginated list with pre-signed download URLs |

Each handler's `cmd/<handler>/main.go` is a thin Wire-injected
`lambda.Start(...)` wrapper. The handler functions themselves live
in `internal/handlers/` and are **shared with the local server**
in [`../CLAUDE.md`](../CLAUDE.md) — only the entry point differs.

## Build

```bash
# Build all handler binaries
make build

# Output:
#   bin/auth.zip
#   bin/telemetry_ingest.zip
#   bin/alert_router.zip
#   bin/presign_upload.zip
#   bin/media_list.zip

# Upload to the deploy S3 bucket (consumed by CloudFormation)
make upload DEPLOY_BUCKET=mobturret-lambda-<env> AWS_REGION=<region>

# Run the test suite
make test
```

`Makefile` should:
- target `linux/amd64` (or `linux/arm64` — open question) with CGO disabled
- strip binaries (`-ldflags="-s -w"`) to keep zips small
- zip each binary at the root (not under a folder) — required by
  Lambda's `Code` zip format

## Local development

Two reasonable options — to be picked:

- **`sam local` / `sam local start-api`** — needs SAM CLI; requires
  a `template.yaml` mirror of the CloudFormation in `../aws/`.
- **`aws-lambda-go` `lambada`/`docker-lambda`** — run the handler
  binary directly in a Lambda-like container; no SAM required.

Either way, each `cmd/<handler>/main.go` should `import` the same
`lambda.Start(handler)` pattern so local + prod entry points match.

## Runtime config

Each handler reads `os.Getenv` keys via `internal/cfg`. Expected
variables (declared in `aws/templates/50-lambdas.yaml`):

| Env var | Used by | Purpose |
|---|---|---|
| `USERS_TABLE` | `auth`, `media_list` | DynamoDB Users table name |
| `TURRETS_TABLE` | `telemetry_ingest`, `alert_router` | DynamoDB Turrets table name |
| `USAGE_LOGS_TABLE` | `telemetry_ingest` | DynamoDB UsageLogs table name |
| `SAVED_MEDIA_TABLE` | `media_list`, `alert_router` | DynamoDB SavedMedia table name |
| `MEDIA_BUCKET` | `presign_upload`, `alert_router`, `media_list` | S3 bucket for alert clips |
| `NOTIFY_TOPIC_ARN` | `alert_router` | SNS topic ARN for push fan-out |
| `JWT_SIGNING_KEY` | `auth`, `media_list`, `presign_upload` | KMS-encrypted; decrypted via `cfg` on cold start |
| `PRE_SIGN_TTL_SECONDS` | `presign_upload`, `media_list` | URL TTL (open question: default 300) |
| `LOG_LEVEL` | all | `debug` / `info` / `warn` / `error` |
| `LOG_FORMAT` | all | `json` (prod) or `console` (local) |

**Local dev uses Viper** to load these from `.env` (see
[`../CLAUDE.md`](../CLAUDE.md)). **Lambda uses raw `os.Getenv`** —
Viper still works in Lambda but adds little value when the
environment is AWS-provided. The two code paths share the
`internal/cfg` struct; only the loader differs.

## Locked dependencies (same as local server)

| Lib | Why |
|---|---|
| `github.com/aws/aws-sdk-go-v2/...` | DynamoDB, S3, SNS, IoT Data |
| `github.com/golang-jwt/jwt/v5` | HS256 JWT issue/verify |
| `golang.org/x/crypto/bcrypt` | Password hashing |
| `go.uber.org/zap` | Structured logging (json encoder in Lambda) |
| `github.com/spf13/viper` | Optional in Lambda — `os.Getenv` is fine; Viper used in local dev |
| `github.com/google/wire` | Wire sets per handler (smaller than the server's set — no HTTP) |
| `github.com/aws/aws-lambda-go/lambda` | `lambda.Start(handler)` entry point |

## Open questions still to settle

- Go version target (1.22? 1.23?)?
- Lambda architecture: `x86_64` (cheaper, ubiquitous) vs `arm64`
  (~20 % cheaper, graviton)?
- Module path: `github.com/<org>/mobturret-server` or local
  `mobturret/server/gun_bot_server`?
- Test framework: standard `testing` + `testify` is idiomatic; do
  we add `gomock` / `aws-sdk-go-v2` mock clients?
- Local dev: SAM CLI or plain container?

## Related docs

- [`../CLAUDE.md`](../CLAUDE.md) — server subproject overview
- [`../aws/CLAUDE.md`](../aws/CLAUDE.md) — CloudFormation templates
  that deploy these handlers
- [`../../design.md`](../../design.md) §4 (cloud infra) and §5
  (DynamoDB) — authoritative spec
