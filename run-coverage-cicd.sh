#!/usr/bin/env bash

#
# -----------------------------------------------------------
#               run-coverage-cicd.sh
#
#  Executes tests and generates coverage reports in formats
#  compatible with SonarQube (XML/LCOV).
#
# -----------------------------------------------------------

set -e

# --- 1. C/C++ Coverage (LCOV) ---
echo "--- Running C/C++ Tests & Coverage ---"
(
    # Create build directory if it doesn't exist
    mkdir -p build_debug
    cd build_debug

    # Configure & Build (Debug mode for coverage flags)
    cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON ..
    cmake --build . -- -j$(nproc)

    # Reset counters
    lcov --directory . --zerocounters -q

    # Run Tests
    ctest --output-on-failure

    # Capture Coverage
    # Added --ignore-errors version to handle GCC 15 vs System GCOV mismatch
    lcov --capture \
         --directory . \
         --output-file coverage.info \
         --ignore-errors version \
         --quiet

    # Filter Artifacts (System libs, Tests, etc)
    # Using exact exclusions from original run-coverage.sh
    lcov --remove coverage.info \
         '/usr/*' \
         '*/_deps/*' \
         '*/tests/helpers.h' \
         '*/benchmark/*' \
         '*/apps/*' \
         '*/docs/*' \
         '*/cmake/*' \
         '*/.cache/*' \
         -o coverage.filtered.info \
         --ignore-errors version \
         --quiet

    # Move to root for easier discovery
    mv coverage.filtered.info ../coverage.cxx.info
)
echo "✅ C/C++ coverage generated: coverage.cxx.info"


# --- 2. Rust Coverage (LCOV) ---
echo "--- Running Rust Tests & Coverage ---"
(
    cd src/rust

    # Generate LCOV report
    # We use --lcov --output-path ...
    cargo llvm-cov --lcov --output-path ../../coverage.rust.info
)
echo "✅ Rust coverage generated: coverage.rust.info"


# --- 3. Python Coverage (XML) ---
echo "--- Running Python Tests & Coverage ---"
(
    # Ensure venv is active
    if [ -d ".venv" ]; then
        . .venv/bin/activate
    fi

    cd src/python

    # Install in editable mode with test deps
    pip install -e .[test] --quiet

    # Run pytest with XML report
    pytest --cov=httppy --cov-report=xml:../../coverage.python.xml tests
)
echo "✅ Python coverage generated: coverage.python.xml"

echo "--- Coverage Complete ---"
ls -lh coverage.*