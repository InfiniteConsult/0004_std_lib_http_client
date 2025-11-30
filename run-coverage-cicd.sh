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

    # Added --ignore-errors version for CI toolchain mismatch
    lcov --directory . --zerocounters

    ctest --output-on-failure

    # Added --ignore-errors version
    lcov --capture \
         --directory . \
         --output-file coverage.info

    # Exact exclusions from run-coverage.sh
    # Added --ignore-errors version
    lcov --remove coverage.info \
         '/usr/*' \
         '*/_deps/*' \
         '*/tests/helpers.h' \
         '*/benchmark/*' \
         '*/apps/*' \
         '*/docs/*' \
         '*/cmake/*' \
         '*/.cache/*' \
         -o coverage.filtered.info

    # Skip genhtml (not needed for SonarQube)

    # Move to root for scanner pickup
    mv coverage.filtered.info ../coverage.cxx.info
)
echo "✅ C/C++ coverage generated: coverage.cxx.info"


# --- 2. Rust Coverage (LCOV) ---
echo "--- Running Rust Tests & Coverage ---"
(
    cd src/rust
    # Changed --html to --lcov
    cargo llvm-cov --lcov --output-path ../../coverage.rust.info
)
echo "✅ Rust coverage generated: coverage.rust.info"


# --- 3. Python Coverage (XML) ---
echo "--- Running Python Tests & Coverage ---"
(
    if [ -d ".venv" ]; then
        . .venv/bin/activate
    fi
    cd src/python
    python3 -m pip install --editable .[test] &> /dev/null

    # Changed --cov-report=html to --cov-report=xml
    pytest -sv --cov=httppy --cov-report=xml:../../coverage.python.xml tests
)
echo "✅ Python coverage generated: coverage.python.xml"

echo "--- Coverage Complete ---"
ls -lh coverage.*