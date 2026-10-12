#!/usr/bin/env bash
# Installs the project-local tools this repository needs but does not vendor.
# Nothing outside the repository is modified and no root access is used; delete
# .tools/ to undo.
#
#   scripts/bootstrap_tools.sh              CMake (only if no system cmake)
#   scripts/bootstrap_tools.sh --profiling  + FlameGraph scripts, matplotlib, cairosvg
#   scripts/bootstrap_tools.sh --force      reinstall CMake even if one exists
#
# Not installed here (system packages, need the machine's owner): g++, make,
# perf (linux-tools), pandoc + a LaTeX engine (xelatex) for the PDF report.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_PIP_VERSION="${CMAKE_PIP_VERSION:-3.31.6}"
# Brendan Gregg's FlameGraph, pinned to the commit the Part A profiles used.
FLAMEGRAPH_URL="https://github.com/brendangregg/FlameGraph"
FLAMEGRAPH_COMMIT="41fee1f99f9276008b7cd112fca19dc3ea84ac32"

want_profiling=no
force=no
for arg in "$@"; do
    case "$arg" in
        --profiling) want_profiling=yes ;;
        --force) force=yes ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

ensure_venv() { [ -x "$ROOT/.tools/venv/bin/pip" ] || python3 -m venv "$ROOT/.tools/venv"; }

if command -v cmake >/dev/null 2>&1 && [ "$force" = no ]; then
    echo "system cmake found: $(command -v cmake) ($(cmake --version | head -1))"
else
    ensure_venv
    "$ROOT/.tools/venv/bin/pip" install --quiet "cmake==${CMAKE_PIP_VERSION}"
    "$ROOT/.tools/venv/bin/cmake" --version | head -1
fi

if [ "$want_profiling" = yes ]; then
    if [ ! -d "$ROOT/.tools/FlameGraph/.git" ]; then
        git clone --quiet "$FLAMEGRAPH_URL" "$ROOT/.tools/FlameGraph"
    fi
    git -C "$ROOT/.tools/FlameGraph" checkout --quiet "$FLAMEGRAPH_COMMIT"
    echo "FlameGraph: $FLAMEGRAPH_URL @ $(git -C "$ROOT/.tools/FlameGraph" rev-parse HEAD)"
    ensure_venv
    "$ROOT/.tools/venv/bin/pip" install --quiet "matplotlib==3.10.9" "cairosvg==2.9.1"
    "$ROOT/.tools/venv/bin/python" -c \
        "import matplotlib, cairosvg; print('matplotlib', matplotlib.__version__, 'cairosvg', cairosvg.__version__)"
fi
