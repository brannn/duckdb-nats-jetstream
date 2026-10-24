PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=nats_js
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# The test runner skips, rather than fails, a test whose extension does not load. CI sets NATS_JS_FAIL_SKIPPED_TESTS,
# so that unittest's --require fails those tests instead.
ifneq ($(NATS_JS_FAIL_SKIPPED_TESTS),)
TEST_RUNNER += --test-command '{binary} {flags} --require nats_js --use-colour yes -f {test_list}'
endif

# Integration tests: test/fixtures/serve.sh runs test/sql/integration against nats-server serving copies of the
# test/fixtures/jetstream store. Locally it needs nats-server on the PATH (or NATS_SERVER=/path/to/nats-server) and a
# release build. CI sets NATS_SERVER_VERSIONS, so that on Linux x86-64 and macOS arm64 the whole test run has servers of
# each of those versions, which serve.sh downloads.
NATS_SERVER ?= nats-server
export NATS_SERVER

.PHONY: test_integration
test_integration:
	@test/fixtures/serve.sh ./build/release/$(TEST_PATH) --require nats_js "$(PROJ_DIR)test/sql/integration/*"

ifneq ($(NATS_SERVER_VERSIONS),)
ifneq ($(filter $(shell uname -s)-$(shell uname -m),Linux-x86_64 Darwin-arm64),)
TEST_RUNNER := $(PROJ_DIR)test/fixtures/serve.sh $(TEST_RUNNER)
endif
endif

# Custom targets for NATS JetStream testing
.PHONY: start stop setup-streams generate-data

start:
	@echo "Starting NATS JetStream..."
	docker compose up -d
	@echo "Waiting for NATS to be healthy..."
	@sleep 5
	@docker compose ps

stop:
	@echo "Stopping NATS JetStream..."
	docker compose down

setup-streams: start
	@echo "Creating JetStream streams..."
	./scripts/setup-streams.sh

generate-data: setup-streams
	@echo "Generating synthetic telemetry data..."
	./scripts/generate-telemetry.py
	protoc --python_out=test/proto -I test/proto test/proto/telemetry.proto
	python3 scripts/generate-telemetry-proto.py

