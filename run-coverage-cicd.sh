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
# We capture from the root so paths like 'src/c/httpc.c' are relative to here,
# not relative to build_debug (which would be '../src/c/httpc.c')
lcov --capture \
     --directory build_debug \
     --output-file coverage.cxx.info \
     --ignore-errors inconsistent,unused,negative \
     --base-directory .

# Filter Artifacts
# Using exact exclusions from run-coverage.sh
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

# Move/Rename final artifact for conversion
# (It's already in root now, so we just rename/use it)
mv coverage.cxx.filtered.info coverage.cxx.info

echo "✅ C/C++ coverage generated: coverage.cxx.info"


# --- 2. Rust Coverage (LCOV) ---
echo "--- Running Rust Tests & Coverage ---"
(
    cd src/rust
    # Changed --html to --lcov
    cargo llvm-cov --lcov --output-path ../../coverage.rust.info
)
echo "✅ Rust coverage generated: coverage.rust.info"


# --- 3. Python Coverage & C++ Conversion (XML) ---
echo "--- Running Python Tests & Converting C++ Report ---"
(
    if [ -d ".venv" ]; then
        . .venv/bin/activate
    fi
    cd src/python

    # Install test dependencies AND the lcov->cobertura converter
    python3 -m pip install --editable .[test] lcov_cobertura --quiet

    # 1. Run Python Tests (Output XML)
    pytest -sv --cov=httppy --cov-report=xml:../../coverage.python.xml tests
    echo "✅ Python coverage generated: coverage.python.xml"

    # 2. Convert C++ LCOV to Cobertura XML
    echo "--- Converting C++ LCOV to Cobertura XML ---"
    # The info file is now at project root (../../coverage.cxx.info)
    # We output the XML to project root (../../coverage.cxx.xml)
    lcov_cobertura ../../coverage.cxx.info --output ../../coverage.cxx.xml
    echo "✅ C/C++ Cobertura XML generated: coverage.cxx.xml"
)

echo "--- Coverage Complete ---"
ls -lh coverage.*