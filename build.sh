#!/bin/sh
# Budowanie: wymaga cmake, ninja (lub make), g++ (C++17) i Qt6 (Widgets, opcjonalnie DBus)
set -e
cd "$(dirname "$0")"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo "Gotowe: build/carton"
