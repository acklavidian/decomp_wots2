#!/usr/bin/env bash
# Decompile functions from SLUS_208.93 with Ghidra + emotionengine-reloaded.
#   tools/ghidra_decomp.sh 0x1107f0 [0x...]     print pseudocode
#   tools/ghidra_decomp.sh --xrefs 0x118728      list references to an address
#   tools/ghidra_decomp.sh --import             (re)create the project, run auto-analysis, apply names
#   tools/ghidra_decomp.sh --names              apply config/functions.json + globals.json to the project
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHIDRA_HOME="${GHIDRA_HOME:-$HOME/opt/ghidra_12.1.4_PUBLIC}"
export JAVA_HOME="${JAVA_HOME:-$(ls -d "$HOME"/opt/jdk-21* | tail -1)}"
export PATH="$JAVA_HOME/bin:$PATH"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
PROJ="$ROOT/ghidra_project"

if [[ "${1:-}" == "--import" ]]; then
    mkdir -p "$PROJ"
    "$HEADLESS" "$PROJ" wots2 -import "$ROOT/extracted/SLUS_208.93" \
        -processor "r5900:LE:32:default" -overwrite
    set -- --names
fi

if [[ "${1:-}" == "--names" ]]; then
    "$HEADLESS" "$PROJ" wots2 -process SLUS_208.93 -noanalysis \
        -scriptPath "$ROOT/tools/ghidra" -postScript ApplyNames.java \
        "$ROOT/config/functions.json" "$ROOT/config/globals.json" 2>&1 \
        | grep -E "ApplyNames.java>|ERROR" || true
    exit 0
fi

SCRIPT=Decompile.java
if [[ "${1:-}" == "--xrefs" ]]; then SCRIPT=Xrefs.java; shift; fi

OUT="$(mktemp)"; LOG="$(mktemp)"
trap 'rm -f "$OUT" "$LOG"' EXIT
if ! "$HEADLESS" "$PROJ" wots2 -process SLUS_208.93 -noanalysis -readOnly \
    -scriptPath "$ROOT/tools/ghidra" -postScript "$SCRIPT" "$OUT" "$@" >"$LOG" 2>&1 \
    || [[ ! -s "$OUT" ]]; then
    cat "$LOG" >&2
    exit 1
fi
cat "$OUT"
