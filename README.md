# cache-sim-rtl-cosim

A **configurable C++ cache simulator** paired with a **synthesizable Verilog RTL cache controller**, connected via a co-simulation framework that cross-validates behavior across 10,000+ memory access traces.

During development, **8 state-machine mismatches** were discovered and fixed between the C++ golden model and the Verilog RTL. See [`docs/mismatch_analysis.md`](docs/mismatch_analysis.md) for a detailed post-mortem.

---

## Features

- **C++ Golden Model**
  - Configurable set-associative cache (direct-mapped through 16-way)
  - LRU and FIFO replacement policies
  - Write-back with write-allocate
  - Detailed statistics: hit rate, miss rate, capacity misses, compulsory misses, conflict misses, evictions
  - Reads standard memory trace files (`R`/`W` format)

- **Verilog RTL (`cache_controller.v`)**
  - Parameterized: `NUM_SETS`, `NUM_WAYS`, `LINE_SIZE`
  - FSM states: `IDLE → TAG_CHECK → HIT / MISS → FILL → EVICT → WRITEBACK`
  - Pseudo-LRU tree tracking (`lru_unit.v`)
  - Write buffer with 4-entry depth

- **Co-Simulation Checker**
  - Testbench (`tb_cache_controller.v`) applies trace vectors to RTL and logs hit/miss/evict per access
  - `CosimChecker` compares C++ golden output vs RTL log
  - Reports mismatches by category with access index, address, and expected vs actual state

- **Trace Generator**
  - Python script (`scripts/gen_traces.py`) generates synthetic traces with configurable working-set size, locality (hot/cold ratio), and R/W mix

---

## Repository Structure

```
cache-sim-rtl-cosim/
├── cpp_model/
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── cache.h
│   │   ├── trace_parser.h
│   │   └── cosim_checker.h
│   └── src/
│       ├── cache.cpp
│       ├── trace_parser.cpp
│       ├── cosim_checker.cpp
│       └── main.cpp
├── rtl/
│   ├── src/
│   │   ├── cache_controller.v
│   │   └── lru_unit.v
│   └── tb/
│       ├── tb_cache_controller.v
│       └── run_sim.sh
├── traces/
│   ├── sample_trace.trace
│   └── failing/
│       └── write_buffer_sync.trace
├── scripts/
│   ├── run_cosim.sh
│   └── gen_traces.py
└── docs/
    └── mismatch_analysis.md
```

---

## Building the C++ Model

### Prerequisites
- CMake ≥ 3.17
- C++17-capable compiler (GCC 9+, Clang 10+, MSVC 2019+)

```bash
cd cpp_model
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The binary is placed at `cpp_model/build/cache_sim`.

### CLI Usage

```
cache_sim [OPTIONS]

Options:
  --trace <file>       Memory trace file (required)
  --sets <N>           Number of cache sets (default: 64)
  --ways <N>           Associativity / number of ways (default: 4)
  --line-size <N>      Cache line size in bytes (default: 64)
  --policy lru|fifo    Replacement policy (default: lru)
  --write-log <file>   Write per-access log for co-sim checker
  --help               Show this help message
```

**Example:**
```bash
./cache_sim --trace ../../traces/sample_trace.trace --sets 64 --ways 4 --line-size 64 --policy lru
```

**Sample output:**
```
Cache Configuration:
  Sets:        64
  Ways:        4
  Line size:   64 bytes
  Total size:  16 KB
  Policy:      LRU, Write-Back/Write-Allocate

Simulation Results (50 accesses):
  Hits:             33  (66.00%)
  Misses:           17  (34.00%)
    Compulsory:     12
    Conflict:        3
    Capacity:        2
  Evictions:        5
  Writebacks:       2
```

---

## Running the RTL Simulation

### Prerequisites
- [Icarus Verilog](http://iverilog.icad.org/) (`iverilog`, `vvp`)
- Optional: GTKWave for waveform viewing

```bash
cd rtl/tb
chmod +x run_sim.sh
./run_sim.sh ../../traces/sample_trace.trace
```

This compiles the RTL + testbench, runs the simulation, and writes `rtl_output.log`.

---

## Running Full Co-Simulation

```bash
cd scripts
chmod +x run_cosim.sh
./run_cosim.sh --trace ../traces/sample_trace.trace --sets 64 --ways 4 --line-size 64
```

The script will:
1. Build the C++ model (if not already built)
2. Run the RTL simulation (produces `rtl_output.log`)
3. Run the C++ model with `--write-log` to produce `cpp_output.log`
4. Invoke the co-sim checker to compare logs
5. Print `PASS` or a mismatch report

### Example Pass Output
```
[COSIM] Comparing 10243 accesses...
[COSIM] PASS — all accesses match between C++ model and RTL.
```

### Example Fail Output
```
[COSIM] MISMATCH at access #847:
  Address:   0x0001F800
  Type:      WRITE
  C++ model: HIT  (dirty eviction pending)
  RTL:       MISS (write buffer not yet flushed)
  Category:  WRITE_BUFFER_SYNC
```

---

## Mismatch History

During validation against a 10,000-entry mixed R/W trace, 8 classes of discrepancy were found. All are now fixed:

| # | Category | Accesses Affected | Fix |
|---|----------|:-----------------:|-----|
| 1 | `WRITE_BUFFER_SYNC` | 312 | Flush write buffer before TAG_CHECK on same line |
| 2 | `LRU_UPDATE_DELAY` | 87 | Move LRU update to end of HIT state, not IDLE |
| 3 | `EVICT_BEFORE_FILL` | 23 | RTL was filling before evicting dirty line |
| 4 | `DIRTY_BIT_CLEAR` | 9 | Dirty bit not cleared after writeback completion |
| 5 | `FIFO_POINTER_WRAP` | 14 | FIFO pointer wrap-around off-by-one |
| 6 | `PARTIAL_WRITE_MASK` | 41 | Byte-enable masking not applied to dirty tracking |
| 7 | `COMPULSORY_VS_CONFLICT` | 3 | Miss classification mismatch (stats only, no functional impact) |
| 8 | `RESET_STATE_MISMATCH` | 1 | RTL initial LRU tree state diverged from C++ model |

See [`docs/mismatch_analysis.md`](docs/mismatch_analysis.md) for root-cause analysis and the exact RTL/C++ diffs.

---

## License

MIT License — see [LICENSE](LICENSE) for details.
