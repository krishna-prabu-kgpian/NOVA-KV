#!/usr/bin/env bash
# publish_part_d.sh <corrected campaign dir>
#
# Makes one campaign the PRIMARY source of the Part D report: aggregates it,
# regroups its profiling data, compares it with the original (wrapped) campaign,
# and copies its summary and figures to results/part_d/summary/ and
# results/part_d/figures/ (the paths the report embeds). Everything written
# here is regenerated from raw files; nothing is edited by hand.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"
C="${1:?usage: publish_part_d.sh results/part_d/unwrapped-...}"
[ -f "$C/CAMPAIGN" ] || { echo "no campaign in $C" >&2; exit 1; }
ORIG="$(ls -d results/part_d/original-wrapped-* | head -1)"
PY=python3; [ -x .tools/venv/bin/python ] && PY=.tools/venv/bin/python
"$PY" scripts/aggregate_part_d.py "$C"
"$PY" scripts/part_d_profile_breakdown.py "$C" > "$C/summary/profile_breakdown.txt"
rm -rf results/part_d/summary results/part_d/figures
cp -r "$C/summary" results/part_d/summary
cp -r "$C/figures" results/part_d/figures
MID="$(for d in results/part_d/unwrapped-*; do [ "$d" = "$C" ] || echo "$d"; done | head -1)"
"$PY" scripts/compare_part_d_campaigns.py "$ORIG" "$MID" "$C" results/part_d/summary
basename "$C" > results/part_d/PRIMARY
cat > results/part_d/README.md <<TXT
# results/part_d — Part D evidence

| Directory | What it is | Use |
|---|---|---|
| \`$(basename "$C")/\` | **Corrected campaign** \`$(cat "$C/CAMPAIGN")\`: timing runs started directly (no profiler); separate profiling track under \`profiling/\` | the source of every Part D result |
| \`$(basename "$ORIG")/\` | Original campaign \`$(cat "$ORIG/CAMPAIGN")\`: every timed run wrapped in \`perf stat\` | audit only; superseded for timing (decisions D-070, D-071). Identical to \`results/part_d/\` at tag \`part-d-final\` |
$(for d in results/part_d/unwrapped-*; do [ "$d" = "$C" ] || echo "| \`$(basename "$d")/\` | Superseded unwrapped campaign \`$(cat "$d/CAMPAIGN")\`: timing unwrapped, but the lock objects' cache-line placement was not controlled | audit only: it is the campaign that exposed the placement effect (decision D-072) |"; done)
| \`summary/\`, \`figures/\` | copies of the corrected campaign's \`summary/\` and \`figures/\`, plus \`summary/original_vs_corrected.md\` | what the report embeds |

\`PRIMARY\` names the corrected campaign. Regenerate with \`scripts/publish_part_d.sh results/part_d/$(basename "$C")\`.
Inside the corrected campaign: \`d1/ d2/ d3/\` are the TIMING track; \`profiling/perf_stat\`, \`profiling/flamegraphs\`,
\`profiling/c2c\` are the PROFILING track (never a source of throughput or latency).
TXT
echo "published $C"
