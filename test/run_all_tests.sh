#!/bin/bash
#
# Automated test runner for NATS JetStream Extension
# Runs all SQL tests with proper setup and teardown
#

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}NATS JetStream Extension Test Suite${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""

# Check prerequisites
echo -e "${YELLOW}Checking prerequisites...${NC}"

command -v docker-compose >/dev/null 2>&1 || {
    echo -e "${RED}Error: docker-compose is required but not installed${NC}"
    exit 1
}

command -v duckdb >/dev/null 2>&1 || {
    echo -e "${RED}Error: duckdb is required but not installed${NC}"
    exit 1
}

command -v python3 >/dev/null 2>&1 || {
    echo -e "${RED}Error: python3 is required but not installed${NC}"
    exit 1
}

echo -e "${GREEN}✓ All prerequisites met${NC}"
echo ""

# Check if extension is built
if [ ! -f "$PROJECT_ROOT/build/release/nats_js.duckdb_extension" ]; then
    echo -e "${YELLOW}Extension not found. Building...${NC}"
    cd "$PROJECT_ROOT"
    make release || {
        echo -e "${RED}Error: Failed to build extension${NC}"
        exit 1
    }
    echo -e "${GREEN}✓ Extension built successfully${NC}"
else
    echo -e "${GREEN}✓ Extension already built${NC}"
fi
echo ""

# Start NATS server
echo -e "${YELLOW}Starting NATS server...${NC}"
cd "$PROJECT_ROOT"
docker-compose up -d

# Wait for NATS to be ready
echo -e "${YELLOW}Waiting for NATS server to be ready...${NC}"
sleep 5

# Verify NATS is accessible
python3 -c "
import asyncio
import sys
try:
    from nats.aio.client import Client as NATS
except ImportError:
    print('nats-py not installed. Install with: pip install nats-py')
    sys.exit(1)

async def check():
    try:
        nc = NATS()
        await nc.connect('nats://localhost:4222', connect_timeout=5)
        await nc.close()
        print('NATS server ready')
        return 0
    except Exception as e:
        print(f'NATS server not ready: {e}')
        return 1

sys.exit(asyncio.run(check()))
" || {
    echo -e "${RED}Error: NATS server not ready${NC}"
    docker-compose down
    exit 1
}

echo -e "${GREEN}✓ NATS server ready${NC}"
echo ""

# Setup JetStream streams
echo -e "${YELLOW}Setting up JetStream streams...${NC}"
"$PROJECT_ROOT/scripts/setup-streams.sh" > /dev/null 2>&1 || {
    echo -e "${YELLOW}Warning: Stream setup may have failed (might already exist)${NC}"
}
echo -e "${GREEN}✓ Streams configured${NC}"
echo ""

# Generate test data
echo -e "${YELLOW}Generating test data...${NC}"

echo -e "${YELLOW}  - Generating JSON telemetry data (2 hours)...${NC}"
cd "$PROJECT_ROOT"
python3 scripts/generate-telemetry.py --hours 2 > /dev/null 2>&1 || {
    echo -e "${YELLOW}Warning: Telemetry generation may have issues${NC}"
}

echo -e "${YELLOW}  - Generating Protobuf test data...${NC}"
python3 test/proto/generate_protobuf_data.py > /dev/null 2>&1 || {
    echo -e "${YELLOW}Warning: Protobuf generation may have issues${NC}"
}

echo -e "${GREEN}✓ Test data generated${NC}"
echo ""

# Arrays to track test results
declare -a PASSED_TESTS
declare -a FAILED_TESTS
declare -a SKIPPED_TESTS

# Function to run a single test
run_test() {
    local test_file=$1
    local test_name=$(basename "$test_file")
    local log_file="/tmp/nats_test_${test_name}.log"

    echo ""
    echo -e "${BLUE}=========================================${NC}"
    echo -e "${BLUE}Running: $test_name${NC}"
    echo -e "${BLUE}=========================================${NC}"

    if duckdb -unsigned :memory: < "$test_file" > "$log_file" 2>&1; then
        echo -e "${GREEN}✓ PASSED: $test_name${NC}"
        PASSED_TESTS+=("$test_name")
        return 0
    else
        echo -e "${RED}✗ FAILED: $test_name${NC}"
        echo -e "${YELLOW}  Log file: $log_file${NC}"
        echo -e "${YELLOW}  Last 20 lines of output:${NC}"
        tail -n 20 "$log_file" | sed 's/^/  /'
        FAILED_TESTS+=("$test_name")
        return 1
    fi
}

# Run all test files
echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}Executing Test Suites${NC}"
echo -e "${BLUE}=========================================${NC}"

cd "$PROJECT_ROOT"

# Test order (logical progression)
# Note: .test files use DuckDB's test framework syntax and are skipped
TEST_FILES=(
    "test/sql/test_json_extraction.sql"
    "test/sql/test_timestamp_queries.sql"
    "test/sql/test_sequence_ranges.sql"
    "test/sql/test_subject_filtering.sql"
    "test/sql/test_protobuf.sql"
    "test/sql/test_protobuf_errors.sql"
    "test/sql/test_payload_blob.sql"
    "test/sql/test_connection_errors.sql"
)

for test_file in "${TEST_FILES[@]}"; do
    if [ -f "$test_file" ]; then
        run_test "$test_file" || true  # Continue even if test fails
    else
        echo -e "${YELLOW}⊘ SKIPPED: $(basename $test_file) (file not found)${NC}"
        SKIPPED_TESTS+=("$(basename $test_file)")
    fi
done

# Cleanup
echo ""
echo -e "${YELLOW}Cleaning up...${NC}"
docker-compose down > /dev/null 2>&1
echo -e "${GREEN}✓ Cleanup complete${NC}"

# Summary
echo ""
echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}TEST SUMMARY${NC}"
echo -e "${BLUE}=========================================${NC}"
echo -e "${GREEN}Passed:  ${#PASSED_TESTS[@]}${NC}"
echo -e "${RED}Failed:  ${#FAILED_TESTS[@]}${NC}"
echo -e "${YELLOW}Skipped: ${#SKIPPED_TESTS[@]}${NC}"
echo ""

if [ ${#PASSED_TESTS[@]} -gt 0 ]; then
    echo -e "${GREEN}Passed tests:${NC}"
    for test in "${PASSED_TESTS[@]}"; do
        echo -e "  ${GREEN}✓${NC} $test"
    done
    echo ""
fi

if [ ${#FAILED_TESTS[@]} -gt 0 ]; then
    echo -e "${RED}Failed tests:${NC}"
    for test in "${FAILED_TESTS[@]}"; do
        echo -e "  ${RED}✗${NC} $test"
    done
    echo ""
fi

if [ ${#SKIPPED_TESTS[@]} -gt 0 ]; then
    echo -e "${YELLOW}Skipped tests:${NC}"
    for test in "${SKIPPED_TESTS[@]}"; do
        echo -e "  ${YELLOW}⊘${NC} $test"
    done
    echo ""
fi

# Exit code
if [ ${#FAILED_TESTS[@]} -eq 0 ]; then
    echo -e "${GREEN}=========================================${NC}"
    echo -e "${GREEN}✓ All tests passed!${NC}"
    echo -e "${GREEN}=========================================${NC}"
    exit 0
else
    echo -e "${RED}=========================================${NC}"
    echo -e "${RED}✗ Some tests failed${NC}"
    echo -e "${RED}=========================================${NC}"
    echo ""
    echo -e "${YELLOW}Check log files in /tmp/nats_test_*.log for details${NC}"
    exit 1
fi
