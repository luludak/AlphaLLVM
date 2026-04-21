#!/usr/bin/env bash
# alpha-run.sh — Full pipeline: .alpha -> native executable -> run
#
# Usage:
#   ./alpha-run.sh program.alpha          # compile + run
#   ./alpha-run.sh --keep program.alpha   # keep intermediate .ll / .o
#   ./alpha-run.sh --ir program.alpha     # only emit .ll, don't run
#
# Prerequisites:
#   alphac, llc, clang, libalpharuntime.a  (from make or cmake build)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ALPHAC="${SCRIPT_DIR}/alphac"
RUNTIME="${SCRIPT_DIR}/libalpharuntime.a"

# Try build dir too
if [ ! -f "$ALPHAC" ]; then
    ALPHAC="${SCRIPT_DIR}/build/alphac"
    RUNTIME="${SCRIPT_DIR}/build/libalpharuntime.a"
fi
if [ ! -f "$ALPHAC" ]; then
    echo "Error: cannot find alphac. Run 'make' or 'cmake --build build' first." >&2
    exit 1
fi

KEEP=0
IR_ONLY=0
INPUT=""
EXTRA_ALPHAC_FLAGS=""

for arg in "$@"; do
    case "$arg" in
        --keep)     KEEP=1 ;;
        --ir)       IR_ONLY=1 ;;
        --dump-ast) EXTRA_ALPHAC_FLAGS="$EXTRA_ALPHAC_FLAGS --dump-ast" ;;
        --dump-symbols) EXTRA_ALPHAC_FLAGS="$EXTRA_ALPHAC_FLAGS --dump-symbols" ;;
        -*)         echo "Unknown flag: $arg" >&2; exit 1 ;;
        *)          INPUT="$arg" ;;
    esac
done

if [ -z "$INPUT" ]; then
    echo "Usage: $0 [--keep] [--ir] [--dump-ast] [--dump-symbols] <file.alpha>" >&2
    exit 1
fi

BASE="${INPUT%.alpha}"
LL="${BASE}.ll"
OBJ="${BASE}.o"
EXE="${BASE}"

# ── Phase 1-4: Alpha -> LLVM IR ──────────────────────────────────
echo "==> Compiling ${INPUT} -> ${LL}"
"$ALPHAC" --emit-llvm $EXTRA_ALPHAC_FLAGS -o "$LL" "$INPUT"

if [ "$IR_ONLY" -eq 1 ]; then
    echo "==> LLVM IR written to ${LL}"
    exit 0
fi

# ── Phase 5a: LLVM IR -> object file ─────────────────────────────
echo "==> Assembling ${LL} -> ${OBJ}"
llc -filetype=obj -o "$OBJ" "$LL"

# ── Phase 5b: Link with runtime ──────────────────────────────────
echo "==> Linking ${OBJ} + runtime -> ${EXE}"
clang "$OBJ" "$RUNTIME" -lm -o "$EXE"

# ── Run ──────────────────────────────────────────────────────────
echo "==> Running ${EXE}"
echo "────────────────────────────────"
"$EXE"
echo "────────────────────────────────"

# ── Cleanup ──────────────────────────────────────────────────────
if [ "$KEEP" -eq 0 ]; then
    rm -f "$LL" "$OBJ"
fi
