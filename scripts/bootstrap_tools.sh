#!/usr/bin/env bash
# Installs a project-local CMake (with CTest) into .tools/venv when no system
# cmake is available and root access is not. Nothing outside the repository is
# modified; delete .tools/ to undo. A system cmake >= 3.16 works equally well.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_PIP_VERSION="${CMAKE_PIP_VERSION:-3.31.6}"

if command -v cmake >/dev/null 2>&1 && [ "${1:-}" != "--force" ]; then
    echo "system cmake found: $(command -v cmake) ($(cmake --version | head -1))"
    exit 0
fi
python3 -m venv "$ROOT/.tools/venv"
"$ROOT/.tools/venv/bin/pip" install --quiet "cmake==${CMAKE_PIP_VERSION}"
"$ROOT/.tools/venv/bin/cmake" --version | head -1
"$ROOT/.tools/venv/bin/ctest" --version | head -1
