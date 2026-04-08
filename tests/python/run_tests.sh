#!/usr/bin/env bash
# Quick script to build and run physics tests
# Works on macOS (zsh/bash) and Linux

set -e

echo "🔧 Setting up Python physics tests..."

# Check if pybind11 is installed
if ! python3 -c "import pybind11" 2>/dev/null; then
    echo "📦 Installing pybind11..."
    pip3 install pybind11
fi

# Check if pytest is installed
if ! python3 -c "import pytest" 2>/dev/null; then
    echo "📦 Installing pytest..."
    pip3 install pytest numpy
fi

# Build the Python bindings
echo "🛠️  Building Python bindings..."
cd ../../build
cmake .. -DBUILD_PYTHON_BINDINGS=ON
make ohao_physics_py -j8

# Run the tests
echo "🧪 Running physics tests..."
cd ../tests/python
export PYTHONPATH=".:$PYTHONPATH"
pytest -v --tb=short

echo "✅ Tests complete!"
