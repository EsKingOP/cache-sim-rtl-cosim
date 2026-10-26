#!/usr/bin/env bash
# run_sim.sh - Compile and run RTL testbench with Icarus Verilog
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RTL_DIR="$SCRIPT_DIR/../src"
BUILD_DIR="$SCRIPT_DIR/../../build/rtl"
mkdir -p "$BUILD_DIR"

TRACE_FILE="${1:-"$SCRIPT_DIR/../../traces/sample_trace.trace"}"
OUTPUT_LOG="${2:-"$BUILD_DIR/rtl_sim.log"}"

echo "[*] Compiling Verilog RTL and testbench with iverilog..."
iverilog -g2012 -o "$BUILD_DIR/cache_tb" \
    "$RTL_DIR/lru_unit.v" \
    "$RTL_DIR/cache_controller.v" \
    "$SCRIPT_DIR/tb_cache_controller.v"

echo "[*] Running simulation on trace: $TRACE_FILE..."
vvp "$BUILD_DIR/cache_tb" "+TRACE=$TRACE_FILE" "+LOG=$OUTPUT_LOG"

echo "[+] RTL simulation completed. Log written to $OUTPUT_LOG"
