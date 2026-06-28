# server — Cloud backend subproject

**Status: planned.** This folder is a placeholder; nothing is built
yet.

Serverless AWS backend for MobTurret. Receives telemetry and alerts
from the edge robot via AWS IoT Core, fans them out to the Flutter
mobile app via push notifications, and exposes a REST API for the
mobile app's auth, device management, and media gallery.

## Layout

| Subfolder | Role |
|---|---|
| [`./aws/`](./aws/) | **CloudFormation** templates — every AWS resource (API Gateway, Lambdas, DynamoDB tables, IoT things, SNS topics, S3 buckets, IAM roles, etc.) is declared here as code. |
| [`./gun_bot_server/`](./gun_bot_server/) | **Go Lambda functions** — `go.mod` workspace, one binary per handler (auth, telemetry ingest, alert router, presign upload, media list, …). Source of truth for handler behaviour. |

CloudFormation references Go binaries via S3 — the build pipeline
zips each handler and uploads to a deploy bucket; the templates
reference that bucket. See [`./aws/CLAUDE.md`](./aws/) for the
exact `Code` wiring and [`./gun_bot_server/CLAUDE.md`](./gun_bot_server/)
for the build / upload flow.

## Responsibilities (per `design.md` §4)

- **API Gateway + Lambda** — JWT-authenticated REST API for the
  Flutter app (login, list turrets, fetch telemetry, list saved
  media).
- **DynamoDB** — tables for `Users`, `Turrets`, `UsageLogs`,
  `SavedMedia` (schemas in `design.md` §5).
- **S3** — short-video alert clips uploaded from the A55 via
  pre-signed URLs.
- **IoT Core + Rule Engine** — MQTT topic ingest, route alert
  events to a notification Lambda.
- **SNS → FCM / APNs** — push notifications when an alert fires.

## Lambda handlers

| Handler | Trigger | Role |
|---|---|---|
| `auth` | API Gateway | Issue / validate JWTs (HS256, shared secret) for the Flutter app |
| `telemetry_ingest` | IoT Rule (`turret/+/telemetry`) | Persist telemetry to DynamoDB |
| `alert_router` | IoT Rule (`turret/+/evt/alert`) | Read turret config, kick off S3 pre-sign + push notification |
| `presign_upload` | API Gateway | Hand the A55 a pre-signed S3 PUT URL for video clip upload |
| `media_list` | API Gateway | Query `SavedMedia` and return pre-signed GET URLs |

See [`./gun_bot_server/CLAUDE.md`](./gun_bot_server/) for the
per-handler code map.

## Local dev runtime

- **HTTP framework:** Gin
- **Config loader:** **Viper** (`github.com/spf13/viper`) — reads
  `.env` (dev) and environment variables (prod / Lambda). Live
  reloading in dev via `viper.WatchConfig()`. Single source of truth
  for `PORT`, `JWT_SIGNING_KEY`, `DYNAMODB_ENDPOINT`, table names,
  log level, etc.
- **Logger:** **Zap** (`go.uber.org/zap`) — structured, leveled,
  high-performance. JSON encoder in prod (`LOG_FORMAT=json`),
  console encoder in dev (`LOG_FORMAT=console`). Every request gets
  a request ID via Gin middleware; handlers log via
  `logger.With(zap.String("request_id", id))`.
- **DI:** **Google Wire** (`github.com/google/wire`) — compile-time
  DI. Provider functions in `internal/wire/` declare the
  dependency graph; `wire ./cmd/server` generates `wire_gen.go` at
  build time. No runtime reflection, no service locators.
- **DynamoDB:** DynamoDB Local in Docker on **port 8181** (no admin
  UI; use `aws dynamodb …` against the local endpoint to inspect)
- **JWT:** HS256, shared secret from `JWT_SIGNING_KEY` (loaded by
  Viper)
- **DynamoDB client:** `aws-sdk-go-v2/service/dynamodb`, endpoint
  URL pointed at `http://localhost:8181` in dev via Viper

## Module layout (planned for M1)

```
server/
├── cmd/
│   └── server/                       # main.go: wire.Build + gin.Run
├── internal/
│   ├── wire/                         # Wire provider sets
│   │   ├── config.go                 # Viper-backed Config struct
│   │   ├── logger.go                 # Zap logger provider
│   │   ├── db.go                     # DynamoDB client provider
│   │   └── server.go                 # *gin.Engine provider
│   ├── config/                       # Config struct + Viper defaults
│   ├── logger/                       # Zap factory + helpers
│   ├── handlers/                     # Gin handler funcs (auth, turrets, …)
│   ├── middleware/                   # auth (JWT), request-id, logging
│   └── db/                           # DynamoDB repos (users, turrets, commands)
├── .env.example                      # committed; documents every Viper key
├── go.mod
├── go.sum
└── Makefile                          # build / test / wire / run
```

**Same code, two runtimes.** The handlers in `internal/handlers/`
are written against `internal/wire`-provided dependencies. The
local server wires them into a Gin engine; the Lambda runtime
(`server/gun_bot_server/cmd/<handler>/main.go`) re-wraps the same
handler functions as `lambda.Start(...)` with a stripped-down Wire
set (no HTTP, no Viper — Lambda env vars + `os.Getenv` instead).
See [`./gun_bot_server/CLAUDE.md`](./gun_bot_server/CLAUDE.md).

## Open questions still to settle

- Auth: Cognito user pool vs custom JWT issuer (the `auth` Lambda
  will implement whichever we pick) — deferred to Phase 2
- Single-region or multi-region with CloudFront in front — Phase 2
- Pre-signed URL TTL for video upload (current draft: 5 min) —
  Phase 2
- DynamoDB single-table or multi-table (`design.md` mentions both) —
  Phase 2
- Alert-clip S3 lifecycle (delete after N days?) — Phase 2

## Locked dependencies (Phase 1 + Phase 2)

| Lib | Why |
|---|---|
| `github.com/gin-gonic/gin` | HTTP framework (local dev) |
| `github.com/spf13/viper` | Config from `.env` + env vars |
| `go.uber.org/zap` | Structured logging |
| `github.com/google/wire` | Compile-time DI |
| `github.com/aws/aws-sdk-go-v2/...` | DynamoDB, S3, SNS, IoT Data |
| `github.com/golang-jwt/jwt/v5` | HS256 JWT issue/verify |
| `golang.org/x/crypto/bcrypt` | Password hashing |
| `github.com/aws/aws-lambda-go/lambda` | Lambda runtime for Phase 2 |

## Related docs

- [`../CLAUDE.md`](../CLAUDE.md) — MobTurret root
- [`../design.md`](../design.md) §4 (cloud infra) and §5 (DynamoDB)
  — authoritative spec
