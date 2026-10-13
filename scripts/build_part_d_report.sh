#!/usr/bin/env bash
# Builds reports/part_d_report.pdf from reports/part_d_report.md (pandoc +
# xelatex). The figures it embeds are results/part_d/figures/*.png, generated
# by scripts/aggregate_part_d.py from the raw campaign data.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT/reports"
pandoc part_d_report.md -o part_d_report.pdf --pdf-engine=xelatex --from markdown+pipe_tables+implicit_figures \
    -V documentclass=extarticle -V fontsize=9pt -V papersize=a4 \
    -V geometry:margin=1.5cm -V colorlinks=true -V linkcolor=black -V urlcolor=black \
    -H part_a_report.header.tex
echo "reports/part_d_report.pdf: $(pdfinfo part_d_report.pdf | awk '/^Pages/ {print $2}') pages"
