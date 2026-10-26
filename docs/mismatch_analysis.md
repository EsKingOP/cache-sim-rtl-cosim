# C++ Cache Simulator vs. Verilog RTL: Mismatch Analysis

During regression verification over **10,000+** memory-access traces, automated co-simulation identified **8 distinct state-machine and microarchitectural mismatches** between the high-level C++ reference model and the synthesizable Verilog RTL.

---

## Catalog of 8 Identified & Resolved Mismatches

### 1. Write-Buffer Synchronization (RAW Hazard)
- **Category**: `WRITE_BUFFER_SYNC`
- **Symptom**: Read directly after write to the same cache line index produced stale data or an unnecessary compulsory miss in RTL.
- **Root Cause**: The C++ model performed atomic updates on write-allocate, whereas the RTL controller queued writes into a pending write-buffer that was not snooped by the tag comparison logic during the `ST_TAG_CHECK` state.
- **Fix**: Added forward-snooping comparator across pending write-buffer entries in RTL tag matching stage.

### 2. LRU State Update 1-Cycle Pipeline Delay
- **Category**: `LRU_UPDATE_DELAY`
- **Symptom**: Back-to-back hits to different ways in the same set evicted incorrect victim ways in subsequent misses.
- **Root Cause**: RTL pseudo-LRU tree updated synchronously on the clock edge following `ST_HIT`, causing back-to-back accesses on consecutive cycles to read pre-update LRU state.
- **Fix**: Pipelined the LRU update bypass logic in `rtl/src/lru_unit.v`.

### 3. Premature Line Eviction Before Fill Handshake
- **Category**: `EVICT_BEFORE_FILL`
- **Symptom**: Cache data corruption during bus stall scenarios.
- **Root Cause**: The RTL state machine transitioned to `ST_EVICT` prior to confirming `mem_ack` for the ongoing line fill request.
- **Fix**: Split the eviction and line fill sequences with explicit acknowledgment handshake states.

### 4. Dirty Bit Retention Post Writeback
- **Category**: `DIRTY_BIT_CLEAR`
- **Symptom**: Clean replaced lines were repeatedly written back to main memory, degrading memory bus throughput.
- **Root Cause**: RTL tag array omitted resetting the `dirty[set][way]` bit register upon completing `ST_WRITEBACK`.
- **Fix**: De-asserted dirty bit in RTL upon receiving `mem_ack` for memory write operations.

### 5. FIFO Victim Pointer Wrap-Around Off-By-One
- **Category**: `FIFO_POINTER_WRAP`
- **Symptom**: Set-associative FIFO replacement skipped way 0 upon wraparound when `NUM_WAYS=4`.
- **Root Cause**: Non-blocking assignment indexing used `<=` with `(head + 1) % NUM_WAYS` evaluated in the wrong sub-cycle.
- **Fix**: Corrected modulo counter width and non-blocking register evaluation logic.

### 6. Partial-Write Byte Enable Masking
- **Category**: `PARTIAL_WRITE_MASK`
- **Symptom**: Byte writes (`SB`, `SH`) did not flag the entire 64-byte line as dirty.
- **Root Cause**: RTL conditional dirty-marking only checked full word `cpu_be == 4'b1111`.
- **Fix**: Updated condition to `|cpu_be` (any active byte enable bit marks the cache line dirty).

### 7. Compulsory vs. Capacity Miss Classification Discrepancy
- **Category**: `COMPULSORY_VS_CONFLICT`
- **Symptom**: C++ and RTL miss classification counters differed by 2.4%.
- **Root Cause**: RTL lacked a cold-start bit vector per tag slot, incorrectly tallying cold misses as conflict misses once index slots were addressed.
- **Fix**: Synchronized cold-miss tracking logic between the reference C++ model and RTL stats registers.

### 8. Post-Reset Asynchronous State Divergence
- **Category**: `RESET_STATE_MISMATCH`
- **Symptom**: Deterministic traces produced divergent victim choices for the first 16 accesses after reset.
- **Root Cause**: C++ model zeroed replacement state, whereas RTL left pseudo-LRU nodes uninitialized (holding indeterminate registers).
- **Fix**: Added synchronous reset loop across all tree node registers in `lru_unit.v`.
