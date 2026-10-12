#!/usr/bin/env bash
# Source-version manifest: SHA-256 of every git-tracked file, in path order,
# preceded by the commit it was taken from. Deterministic for a given commit.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"
echo "# git_sha: $(git rev-parse HEAD)"
echo "# git_dirty: $(nova_git_dirty)"
git ls-files -z | LC_ALL=C sort -z | xargs -0 sha256sum
