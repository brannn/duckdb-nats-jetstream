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

# Integration tests: serve a copy of the test/fixtures/jetstream store and run test/sql/integration against it.
# Needs nats-server on the PATH (or NATS_SERVER=/path/to/nats-server) and a release build.
NATS_SERVER ?= nats-server
NATS_JS_TEST_PORT ?= 42222
NATS_JS_TEST_STORE := build/nats-test-store

.PHONY: test_integration
test_integration:
	@rm -rf $(NATS_JS_TEST_STORE) && mkdir -p $(NATS_JS_TEST_STORE) && cp -R test/fixtures/jetstream $(NATS_JS_TEST_STORE)/
	@$(NATS_SERVER) -js -a 127.0.0.1 -p $(NATS_JS_TEST_PORT) -sd $(NATS_JS_TEST_STORE) > $(NATS_JS_TEST_STORE)/server.log 2>&1 & \
	server=$$!; \
	for i in $$(seq 50); do grep -q "Server is ready" $(NATS_JS_TEST_STORE)/server.log && break; sleep 0.1; done; \
	NATS_JS_TEST_URL=nats://127.0.0.1:$(NATS_JS_TEST_PORT) ./build/release/$(TEST_PATH) --require nats_js "$(PROJ_DIR)test/sql/integration/*"; \
	status=$$?; kill $$server; exit $$status

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

