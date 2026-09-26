#!/usr/bin/env bash
set -euo pipefail

# Run from the MSYS2 UCRT64 shell. The KDE library in MSYS2 is shared, so
# build its static variant with definitions embedded before linking og.
if ! command -v git >/dev/null; then
    echo "git is needed to fetch KDE syntax-highlighting (pacman -S git)" >&2
    exit 1
fi
root="$(cd "$(dirname "$0")/.." && pwd)"
source_dir="$root/build-syntax-source"
syntax_build="$root/build-syntax-build"
syntax_prefix="$root/build-syntax-install"

if [[ ! -f "$source_dir/CMakeLists.txt" ]]; then
    git clone --depth 1 --branch v6.30.0 https://github.com/KDE/syntax-highlighting.git "$source_dir"
fi

cmake -S "$source_dir" -B "$syntax_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
    -DQRC_SYNTAX=ON -DCMAKE_PREFIX_PATH="/ucrt64/qt6-static;/ucrt64" \
    -DCMAKE_INSTALL_PREFIX="$syntax_prefix"
cmake --build "$syntax_build" --parallel 4
cmake --install "$syntax_build"

cmake -S "$root" -B "$root/build-standalone" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DOG_STANDALONE=ON \
    -DKF6SyntaxHighlighting_DIR="$syntax_prefix/lib/cmake/KF6SyntaxHighlighting" \
    -DCMAKE_PREFIX_PATH="/ucrt64/qt6-static;$syntax_prefix"
cmake --build "$root/build-standalone" --parallel 4
