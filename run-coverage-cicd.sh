#!/usr/bin/env bash

#
# -----------------------------------------------------------
#               run-coverage-cicd.sh
#
#  Adapted from run-coverage.sh for CI/CD.
#  Generates XML/LCOV reports instead of HTML.
#
# -----------------------------------------------------------

set -e

# --- 1. C/C++ Coverage (LCOV) ---
echo "--- Running C/C++ Tests & Coverage ---"
(
    mkdir -p build_debug
    cd build_debug || exit

    # Ensure Debug build for coverage flags
    cmake -DCMAKE_BUILD_TYPE=Debug ..
    cmake --build . -- -j$(nproc)

    # Reset counters
    lcov --directory . --zerocounters

    ctest --output-on-failure

    # Capture Coverage
    lcov --capture \
         --directory . \
         --output-file coverage.info \
         --ignore-errors inconsistent,unused,negative

    # Exact exclusions from run-coverage.sh
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
         --ignore-errors inconsistent,unused,negative

    # We leave coverage.filtered.info here in build_debug
)
echo "✅ C/C++ coverage generated: build_debug/coverage.filtered.info"


# --- 2. Rust Coverage (LCOV) ---
echo "--- Running Rust Tests & Coverage ---"
(
    cd src/rust
    # Changed --html to --lcov
    cargo llvm-cov --lcov --output-path ../../coverage.rust.info
)
echo "✅ Rust coverage generated: coverage.rust.info"


# --- 3. Python Coverage (XML) ---
echo "--- Running Python Tests & Installing Tools ---"
(
    if [ -d ".venv" ]; then
        . .venv/bin/activate
    fi
    cd src/python

    # Install test dependencies AND the lcov->cobertura converter
    python3 -m pip install --editable .[test] lcov_cobertura --quiet

    # 1. Run Python Tests (Output XML)
    pytest -sv --cov=httppy --cov-report=xml:../../coverage.python.xml tests
)
echo "✅ Python coverage generated: coverage.python.xml"


# --- 4. Convert C++ LCOV to Cobertura XML ---
echo "--- Converting C++ LCOV to Cobertura XML ---"
(
    # Activate venv to access lcov_cobertura
    if [ -d ".venv" ]; then
        . .venv/bin/activate
    fi

    # Go into build directory to maintain relative paths
    cd build_debug

    # Convert the filtered info file generated in Step 1
    lcov_cobertura coverage.filtered.info --output coverage.cxx.xml
)
echo "✅ C/C++ Cobertura XML generated: build_debug/coverage.cxx.xml"

echo "--- Coverage Complete ---"
ls -lh coverage.* build_debug/coverage.cxx.xml