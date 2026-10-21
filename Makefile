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

# Custom targets for NATS JetStream testing
.PHONY: start stop setup-streams generate-data

start:
	@echo "Starting NATS JetStream..."
	docker-compose up -d
	@echo "Waiting for NATS to be healthy..."
	@sleep 5
	@docker-compose ps

stop:
	@echo "Stopping NATS JetStream..."
	docker-compose down

setup-streams: start
	@echo "Creating JetStream streams..."
	./scripts/setup-streams.sh

generate-data: setup-streams
	@echo "Generating synthetic telemetry data..."
	./scripts/generate-telemetry.py

