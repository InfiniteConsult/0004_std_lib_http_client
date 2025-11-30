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

# A. Zero Counters (Run from Root, targeting build dir)
mkdir -p build_debug
lcov --directory build_debug --zerocounters --ignore-errors inconsistent,unused,negative -q

# B. Build & Run Tests (Inside build dir)
(
    cd build_debug || exit
    # Ensure Debug build for coverage flags
    cmake -DCMAKE_BUILD_TYPE=Debug ..
    cmake --build . -- -j$(nproc)
    ctest --output-on-failure
)

# C. Capture & Filter (Run from Root)
# We capture from the root so paths like 'src/c/httpc.c' are relative to here.
lcov --capture \
     --directory build_debug \
     --output-file coverage.cxx.info \
     --ignore-errors inconsistent,unused,negative \
     --base-directory .

# Filter Artifacts
lcov --remove coverage.cxx.info \
     '/usr/*' \
     '*/_deps/*' \
     '*/tests/helpers.h' \
     '*/benchmark/*' \
     '*/apps/*' \
     '*/docs/*' \
     '*/cmake/*' \
     '*/.cache/*' \
     -o coverage.cxx.filtered.info \
     --ignore-errors inconsistent,unused,negative

# Rename for final use
mv coverage.cxx.filtered.info coverage.cxx.info

echo "✅ C/C++ coverage generated: coverage.cxx.info"


# --- 2. Rust Coverage (LCOV) ---
echo "--- Running Rust Tests & Coverage ---"
(
    cd src/rust
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

    # Install lcov_cobertura here (inside the venv)
    python3 -m pip install --editable .[test] lcov_cobertura --quiet

    # Run Python Tests
    pytest -sv --cov=httppy --cov-report=xml:../../coverage.python.xml tests
)
echo "✅ Python coverage generated: coverage.python.xml"


# --- 4. Convert C++ LCOV to Cobertura XML (From Root) ---
echo "--- Converting C++ LCOV to Cobertura XML ---"
(
    # Activate the venv (using relative path from Root) to get access to lcov_cobertura
    if [ -f "src/python/.venv/bin/activate" ]; then
        . src/python/.venv/bin/activate
    elif [ -f ".venv/bin/activate" ]; then
        . .venv/bin/activate
    fi

    # Run conversion from ROOT so paths remain 'src/c/...' (matching the LCOV input)
    lcov_cobertura coverage.cxx.info --output coverage.cxx.xml
)
echo "✅ C/C++ Cobertura XML generated: coverage.cxx.xml"

echo "--- Coverage Complete ---"
ls -lh coverage.*