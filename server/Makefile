# MobTurret server Makefile
#
# Common targets:
#   make help      - print this help
#   make tidy      - go mod tidy
#   make build     - compile to ./bin/server
#   make test      - go test ./... (DDB integration tests need `make ddb-up`; they skip if it's down)
#   make run       - run the server (loads .env if present)
#   make ddb-up    - bring up local DynamoDB via docker compose
#   make ddb-down  - tear down local DynamoDB
#   make verify    - end-to-end smoke test (requires ddb-up + run in another terminal)

.PHONY: help tidy build test test-unit test-integration run ddb-up ddb-down verify clean

BINARY := bin/server
PKG    := ./cmd/server

help:
	@echo "MobTurret server targets:"
	@echo "  make tidy            - go mod tidy"
	@echo "  make build           - compile to $(BINARY)"
	@echo "  make test            - run all tests"
	@echo "  make test-unit       - run tests that don't need DynamoDB Local"
	@echo "  make test-integration- run DynamoDB Local integration tests (needs ddb-up)"
	@echo "  make run             - go run $(PKG)"
	@echo "  make ddb-up          - docker compose up dynamodb"
	@echo "  make ddb-down        - docker compose down dynamodb"
	@echo "  make verify          - curl smoke test (server must be running)"
	@echo "  make clean           - remove build artifacts"

tidy:
	go mod tidy

build:
	@mkdir -p bin
	go build -o $(BINARY) $(PKG)

# All tests, including DDB Local integration tests.
# The DDB tests default to http://localhost:8181 and skip if it's down.
test:
	go test ./...

# Unit-only — no DDB dependency. Useful for fast CI.
test-unit:
	go test ./internal/auth/usecase/... ./internal/auth/adapter/handler/... ./internal/platform/...

# Integration tests against DynamoDB Local. Requires ddb-up.
test-integration:
	go test -v -count=1 ./internal/auth/adapter/repository/...

run:
	go run $(PKG)

ddb-up:
	docker compose up -d dynamodb

ddb-down:
	docker compose down

# End-to-end smoke test. Run from repo root with `make -C server verify`.
# Assumes the server is already running on :8182.
# EMAIL is timestamped so repeat runs don't collide with the 409 check.
verify:
	@echo "==> healthz"
	@curl -sS -o /dev/null -w "  status: %{http_code}\n" http://localhost:8182/healthz

	@EMAIL="verify+$$(date +%s)@example.com"; \
	PW='correct horse battery'; \
	echo "==> signup ($$EMAIL)"; \
	curl -sS -X POST http://localhost:8182/auth/signup \
		-H 'Content-Type: application/json' \
		-d "{\"email\":\"$$EMAIL\",\"password\":\"$$PW\"}" | head -c 200; echo; \
	echo "==> signup duplicate (should be 409)"; \
	curl -sS -o /dev/null -w "  status: %{http_code}\n" -X POST http://localhost:8182/auth/signup \
		-H 'Content-Type: application/json' \
		-d "{\"email\":\"$$EMAIL\",\"password\":\"$$PW\"}"; \
	echo "==> login"; \
	TOKEN=$$(curl -sS -X POST http://localhost:8182/auth/login \
		-H 'Content-Type: application/json' \
		-d "{\"email\":\"$$EMAIL\",\"password\":\"$$PW\"}" \
		| sed -E 's/.*"token":"([^"]+)".*/\1/'); \
	echo "  token (truncated): $${TOKEN:0:30}..."; \
	echo "==> /me with token"; \
	curl -sS -o /dev/null -w "  status: %{http_code}\n" http://localhost:8182/auth/me \
		-H "Authorization: Bearer $$TOKEN"; \
	echo "==> login wrong password (should be 401)"; \
	curl -sS -o /dev/null -w "  status: %{http_code}\n" -X POST http://localhost:8182/auth/login \
		-H 'Content-Type: application/json' \
		-d "{\"email\":\"$$EMAIL\",\"password\":\"definitely-wrong\"}"

	@echo "==> /me without token (should be 401)"
	@curl -sS -o /dev/null -w "  status: %{http_code}\n" http://localhost:8182/auth/me

	@echo "==> GET /turrets without token (should be 401)"
	@curl -sS -o /dev/null -w "  status: %{http_code}\n" http://localhost:8182/turrets

	@echo "==> GET /turrets with token (should be 200, empty array)"
	@curl -sS -w "\n  status: %{http_code}\n" http://localhost:8182/turrets \
		-H "Authorization: Bearer $$TOKEN" | tee /tmp/turrets-resp.json; \
	grep -q '"turrets":\[\]' /tmp/turrets-resp.json || { echo "  FAIL: expected empty turrets array"; exit 1; }

clean:
	rm -rf bin/
