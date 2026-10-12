# common.sh -- sourced by the other scripts; resolves the repo root and cmake.
NOVA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Prefer an explicit $CMAKE, then a system cmake, then the project-local one
# installed by scripts/bootstrap_tools.sh.
nova_resolve_cmake() {
    if [ -n "${CMAKE:-}" ]; then
        NOVA_CMAKE="$CMAKE"
    elif command -v cmake >/dev/null 2>&1; then
        NOVA_CMAKE="$(command -v cmake)"
    elif [ -x "$NOVA_ROOT/.tools/venv/bin/cmake" ]; then
        NOVA_CMAKE="$NOVA_ROOT/.tools/venv/bin/cmake"
    else
        echo "cmake not found: install it or run scripts/bootstrap_tools.sh" >&2
        return 1
    fi
    NOVA_CTEST="$(dirname "$NOVA_CMAKE")/ctest"
    [ -x "$NOVA_CTEST" ] || NOVA_CTEST="$(command -v ctest)"
}

# Prints yes/no. Verification evidence written by verify_all.sh for the commit
# under test does not count as a modification of that commit.
nova_git_dirty() {
    if [ -n "$(git -C "$NOVA_ROOT" status --porcelain -- . ':!results/verification' 2>/dev/null)" ]; then
        echo yes
    else
        echo no
    fi
}
