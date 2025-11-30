#!/usr/bin/env bash

# Create our Python virtual environment. This will be found in the CMakeLists and allow us to automatically build our
# wheel.
if [ ! -d ".venv" ]; then
    python3.12 -m venv .venv;
    . .venv/bin/activate;
    (
        cd src/python || (echo "python source directory not found" && exit)
        python3 -m pip install -r requirements.txt
    )
fi

# --- Build Debug Version ---
echo "Configuring and building Debug version..."
cmake -S . -B build_debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build_debug

# --- Build Release Version ---
echo "Configuring and building Release version..."
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release

echo "Setup complete. Builds are in build_debug/ and build_release/"