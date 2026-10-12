#!/usr/bin/env bash
# Verifies the assignment's limits on the rendered Part A report (PDF p. 3):
# "a report of no more than 5 pages" and a design note of "<= 3 of your 5 pages".
# Page counts come from the PDF itself (pdfinfo / pdftotext), not from the source.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"
PDF="${1:-reports/part_a_report.pdf}"
NOTE_START="Storage layout design note"   # heading that starts the design note
NOTE_END="Limitations and what is not shown"  # heading of the section after it

[ -f "$PDF" ] || { echo "FAIL: $PDF does not exist"; exit 1; }
pages="$(pdfinfo "$PDF" | awk '/^Pages:/ {print $2}')"
echo "report: $PDF"
echo "pages: $pages (limit 5)"
status=0
[ "$pages" -le 5 ] || { echo "FAIL: report exceeds 5 pages"; status=1; }

first=0; last=0
for p in $(seq 1 "$pages"); do
    text="$(pdftotext -f "$p" -l "$p" "$PDF" - 2>/dev/null)"
    if [ "$first" -eq 0 ] && grep -q "$NOTE_START" <<<"$text"; then first="$p"; fi
    if [ "$last" -eq 0 ] && grep -q "$NOTE_END" <<<"$text"; then last="$p"; fi
done
if [ "$first" -eq 0 ] || [ "$last" -eq 0 ]; then
    echo "FAIL: could not locate the design note's start/end headings in the PDF"; exit 1
fi
# Pages the note touches: from the page with its heading through the page on
# which the next section begins (counted in full, which can only overcount).
span=$((last - first + 1))
echo "design note: pages $first-$last, at most $span page(s) (limit 3)"
[ "$span" -le 3 ] || { echo "FAIL: design note exceeds 3 pages"; status=1; }
[ $status -eq 0 ] && echo "PASS"
exit $status
