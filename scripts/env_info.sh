#!/usr/bin/env bash
# Prints the machine / toolchain facts every result must be tied to
# (blueprint 9.3 and 16.4). Read-only; a probe that fails prints why.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

section() { printf '\n== %s ==\n' "$1"; }
probe() { "$@" 2>&1 || echo "(unavailable: '$*' exited $?)"; }

echo "captured_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "git_sha: $(git rev-parse HEAD 2>/dev/null || echo none)"
echo "git_dirty: $([ -n "$(git status --porcelain 2>/dev/null)" ] && echo yes || echo no)"

section "kernel / OS";          probe uname -a; probe grep -E '^(NAME|VERSION)=' /etc/os-release
section "CPU";                  probe lscpu
section "online logical CPUs";  probe nproc
section "NUMA";                 probe sh -c 'lscpu | grep -i numa'
section "memory";               probe free -h
section "cpufreq governor";     probe sh -c 'sort /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | uniq -c'
section "frequency boost";      probe cat /sys/devices/system/cpu/cpufreq/boost
section "compiler";             probe g++ --version
section "cmake"
if nova_resolve_cmake; then echo "$NOVA_CMAKE"; probe "$NOVA_CMAKE" --version; fi
section "make";                 probe sh -c 'make --version | head -1'
section "perf";                 probe perf --version
section "perf_event_paranoid";  probe cat /proc/sys/kernel/perf_event_paranoid
section "perf stat usable (unprivileged)"
if perf stat -e task-clock -- true >/dev/null 2>&1; then echo yes; else echo "no (perf stat -e task-clock -- true exited $?)"; fi
section "FlameGraph scripts"
if command -v stackcollapse-perf.pl >/dev/null 2>&1 && command -v flamegraph.pl >/dev/null 2>&1; then
    command -v stackcollapse-perf.pl flamegraph.pl
else
    echo "not on PATH"
fi
section "other tools"
for t in clang++ valgrind taskset numactl python3 zip unzip; do
    printf '%-10s %s\n' "$t" "$(command -v "$t" 2>/dev/null || echo 'not found')"
done
