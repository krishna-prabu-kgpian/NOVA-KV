#!/usr/bin/env python3
"""Sums the HITM load samples of a `perf c2c report --stdio` by the function that took them.

    scripts/c2c_by_symbol.py results/part_d/perf/c2c/<id>.c2c-report.txt [...]

For each shared cache line the report's Pareto section gives the line's local/remote HITM
count and, per code address, the percentage of it. This multiplies them out, so the output
says which functions' loads were hit by another core's modification, and on how many
distinct cache lines. It adds nothing to the report; it only regroups it.
"""
import re
import sys
from collections import defaultdict

for path in sys.argv[1:]:
    text = open(path, errors="replace").read()
    start = text.find("Shared Cache Line Distribution Pareto")
    if start < 0:
        print(f"{path}: no Pareto section")
        continue
    by_sym = defaultdict(float)
    lines_of = defaultdict(set)
    total = 0.0
    cur_line, cur_hitm = None, 0.0
    for line in text[start:].splitlines():
        m = re.match(r"\s+(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+\d+\s+(0x[0-9a-f]+)\s*$", line)
        if m:
            cur_line, cur_hitm = m.group(4), float(m.group(2)) + float(m.group(3))
            total += cur_hitm
            continue
        m = re.match(r"\s+([\d.]+)%\s+([\d.]+)%\s+[\d.]+%\s+[\d.]+%\s+[\d.]+%\s+(0x[0-9a-f]+).*?\[[.k]\]\s+(\S.*?)\s{2,}(\S+)\s+(\S+)\s+\d+\s*$", line)
        if m and cur_line is not None:
            share = (float(m.group(1)) + float(m.group(2))) / 100.0
            sym = f"{m.group(4)} [{m.group(5)}]"
            by_sym[sym] += share * cur_hitm
            lines_of[sym].add(cur_line)
    print(f"== {path}")
    print(f"   HITM load samples in the Pareto section: {total:.0f}")
    for sym, v in sorted(by_sym.items(), key=lambda kv: -kv[1])[:14]:
        print(f"   {v:8.1f}  {100 * v / total if total else 0:5.1f}%  {len(lines_of[sym]):5d} lines  {sym}")
