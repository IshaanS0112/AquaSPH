# Common tasks; CI (.github/workflows/ci.yml) runs the same commands.
#   make solver test    build the C++ solver and run its 89 tests
#   make backend-test   Go unit + integration tests (needs Postgres and Redis)
#   make e2e            real binaries, real solver, failure injection
#   make up / make down the whole platform in Docker Compose

GO_DIR := backend
BUILD  := build

.PHONY: solver test backend backend-test e2e lint up down demo-key loadtest clean

solver:
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) --parallel

test: solver
	ctest --test-dir $(BUILD) --output-on-failure

backend:
	cd $(GO_DIR) && go build -o bin/ ./cmd/...

backend-test:
	cd $(GO_DIR) && go vet ./... && go test -race -short -count=1 ./...

e2e: solver
	cd $(GO_DIR) && go test -count=1 -timeout 15m -v ./e2e/

lint:
	cd $(GO_DIR) && test -z "$$(gofmt -l . | tee /dev/stderr)" && go vet ./...

up:
	GIT_REV=$$(git rev-parse --short HEAD) docker compose up --build -d --wait

down:
	docker compose down -v

# Creates a tenant called "demo" (if needed) and prints a fresh API key.
demo-key:
	-docker compose exec -T api aquasph-admin tenant create demo >/dev/null 2>&1
	docker compose exec -T api aquasph-admin key create demo

# Measures API overhead against the running stack. Needs AQUASPH_API_KEY
# for a tenant whose rate limit will not throttle the test.
loadtest: backend
	for m in get submit list; do $(GO_DIR)/bin/aquasph-loadgen -mode $$m -c 32 -d 20s; done

clean:
	rm -rf $(BUILD) $(GO_DIR)/bin
