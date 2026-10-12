#!/usr/bin/env bash
# Builds reports/part_a_report.pdf from reports/part_a_report.src.md.
#
#   1. scripts/aggregate_part_a.py regenerates the summary tables and figures
#      from the raw results (skipped with --no-aggregate).
#   2. {{include:<path>}} lines in the source are replaced by the named file,
#      so every table in the report is generated from raw data, not typed in.
#   3. pandoc + xelatex produce the PDF.
#   4. scripts/check_part_a_report.sh verifies the page limits.
#
# Needs pandoc, xelatex, pdfinfo, pdftotext; python with matplotlib + cairosvg
# for step 1 (scripts/bootstrap_tools.sh --profiling).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

PY="$NOVA_ROOT/.tools/venv/bin/python"
[ -x "$PY" ] || PY=python3
if [ "${1:-}" != "--no-aggregate" ]; then
    "$PY" scripts/aggregate_part_a.py results/part_a
fi

SRC=reports/part_a_report.src.md
MD=reports/part_a_report.md
PDF=reports/part_a_report.pdf

"$PY" - "$SRC" "$MD" <<'PYEOF'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
out = []
for line in open(src):
    m = re.match(r"^\{\{include:(.+)\}\}\s*$", line)
    out.append(open(m.group(1)).read() if m else line)
open(dst, "w").write("".join(out))
PYEOF

pandoc "$MD" -o "$PDF" --pdf-engine=xelatex --from markdown+pipe_tables+implicit_figures \
    -V documentclass=extarticle -V fontsize=9pt -V papersize=a4 \
    -V geometry:margin=1.6cm -V colorlinks=true -V linkcolor=black -V urlcolor=black \
    -H reports/part_a_report.header.tex

scripts/check_part_a_report.sh
