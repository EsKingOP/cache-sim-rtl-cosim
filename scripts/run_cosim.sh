#!/usr/bin/env bash
# run_cosim.sh - Orchestrate C++ cache model and RTL co-simulation verification
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT_DIR/build"
TRACE_FILE="${1:-"$ROOT_DIR/traces/sample_trace.trace"}"

ASSOC=4
LINE_SIZE=64
POLICY="LRU"

mkdir -p "$BUILD_DIR"

echo "========================================================================"
echo " Starting Co-Simulation Verification"
echo " Trace: $TRACE_FILE | Assoc: $ASSOC | Line Size: $LINE_SIZE | Policy: $POLICY"
echo "========================================================================"

# 1. Build C++ Model
echo "[*] Step 1: Building C++ golden reference simulator..."
cmake -B "$BUILD_DIR/cpp" -S "$ROOT_DIR/cpp_model" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR/cpp"

# 2. Run C++ Model to generate golden log
echo "[*] Step 2: Executing C++ golden reference model..."
CPP_LOG="$BUILD_DIR/cpp_sim.log"
"$BUILD_DIR/cpp/cache_sim" \
    --trace "$TRACE_FILE" \
    --assoc "$ASSOC" \
    --line-size "$LINE_SIZE" \
    --policy "$POLICY" \
    --log "$CPP_LOG"

# 3. Run RTL Simulation
echo "[*] Step 3: Running Verilog RTL simulation..."
RTL_LOG="$BUILD_DIR/rtl_sim.log"
"$ROOT_DIR/rtl/tb/run_sim.sh" "$TRACE_FILE" "$RTL_LOG"

# 4. Run Co-Simulation Checker
echo "[*] Step 4: Comparing C++ golden log vs RTL output log..."
"$BUILD_DIR/cpp/cache_sim" \
    --check \
    --cpp-log "$CPP_LOG" \
    --rtl-log "$RTL_LOG"

echo "========================================================================"
echo " Co-Simulation Verification SUCCESS: 0 Mismatches"
echo "========================================================================"
