#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>
#include <optional>
#include <string>
#include <array>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
//  Enumerations
// ─────────────────────────────────────────────────────────────────────────────

enum class ReplacementPolicy {
    LRU,   ///< Least-Recently-Used
    FIFO,  ///< First-In First-Out
};

enum class WritePolicy {
    WRITE_BACK,       ///< Write-back with write-allocate
    WRITE_THROUGH,    ///< Write-through with no-write-allocate
};

enum class AccessType {
    READ,
    WRITE,
};

enum class AccessResult {
    HIT,
    MISS_COMPULSORY,   ///< Cold miss — line never loaded
    MISS_CONFLICT,     ///< Line was evicted due to conflict (set full)
    MISS_CAPACITY,     ///< Line was evicted due to overall capacity
};

// ─────────────────────────────────────────────────────────────────────────────
//  Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct CacheConfig {
    std::size_t num_sets   = 64;    ///< Number of sets
    std::size_t num_ways   = 4;     ///< Associativity (ways per set)
    std::size_t line_size  = 64;    ///< Cache line size in bytes (must be power-of-two)
    ReplacementPolicy policy = ReplacementPolicy::LRU;
    WritePolicy write_policy = WritePolicy::WRITE_BACK;

    /// Total capacity in bytes
    std::size_t capacity_bytes() const { return num_sets * num_ways * line_size; }

    /// Offset bits within a line
    int offset_bits() const;
    /// Index bits for set selection
    int index_bits() const;
    /// Tag bits (remainder of address)
    int tag_bits() const;

    /// Validate configuration (powers of two, sensible ranges)
    bool is_valid() const;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Statistics
// ─────────────────────────────────────────────────────────────────────────────

struct CacheStats {
    uint64_t total_accesses   = 0;
    uint64_t reads            = 0;
    uint64_t writes           = 0;
    uint64_t hits             = 0;
    uint64_t misses           = 0;
    uint64_t compulsory_misses = 0;
    uint64_t conflict_misses  = 0;
    uint64_t capacity_misses  = 0;
    uint64_t evictions        = 0;
    uint64_t dirty_evictions  = 0;  ///< Evictions that required a writeback
    uint64_t writebacks       = 0;

    double hit_rate()  const { return total_accesses ? (double)hits  / total_accesses : 0.0; }
    double miss_rate() const { return total_accesses ? (double)misses / total_accesses : 0.0; }

    void print() const;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Internal cache line / set structures
// ─────────────────────────────────────────────────────────────────────────────

struct CacheLine {
    bool     valid = false;
    bool     dirty = false;
    uint64_t tag   = 0;
    uint64_t last_used_time = 0;  ///< For LRU ordering
    uint32_t fifo_order     = 0;  ///< For FIFO ordering
};

struct CacheSet {
    std::vector<CacheLine> ways;
    uint32_t fifo_head = 0;  ///< Next victim index for FIFO

    explicit CacheSet(std::size_t num_ways)
        : ways(num_ways) {}
};

// ─────────────────────────────────────────────────────────────────────────────
//  Per-access log entry (for co-sim)
// ─────────────────────────────────────────────────────────────────────────────

struct AccessLog {
    uint64_t     index;       ///< Sequential access number
    uint64_t     address;
    AccessType   type;
    AccessResult result;
    uint64_t     tag;
    uint32_t     set_index;
    int          way_hit;     ///< Way that hit, or -1 on miss
    int          way_evict;   ///< Way chosen for eviction, or -1
    bool         evicted_dirty;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Cache simulator
// ─────────────────────────────────────────────────────────────────────────────

class Cache {
public:
    explicit Cache(const CacheConfig& config);

    /// Process a single memory access. Returns the result and updates stats.
    AccessResult access(uint64_t address, AccessType type, uint32_t data = 0);

    /// Access with full log output (used by co-sim checker).
    AccessLog access_logged(uint64_t address, AccessType type, uint32_t data = 0);

    const CacheStats&  stats()  const { return stats_; }
    const CacheConfig& config() const { return config_; }

    /// Flush all dirty lines (write-back), returning writeback count.
    std::size_t flush();

    /// Reset to initial state.
    void reset();

    /// Write per-access log to file (for co-sim comparison).
    void write_log(const std::string& path) const;

private:
    // ── Address decomposition ─────────────────────────────────────────────────
    uint64_t extract_tag   (uint64_t addr) const;
    uint32_t extract_index (uint64_t addr) const;
    uint32_t extract_offset(uint64_t addr) const;

    // ── Replacement policy helpers ────────────────────────────────────────────
    int  find_hit_way   (const CacheSet& set, uint64_t tag) const;
    int  find_victim    (const CacheSet& set) const;
    void update_lru     (CacheSet& set, int used_way);
    int  insert_line    (CacheSet& set, uint64_t tag, bool dirty,
                         int* evict_way_out, bool* evict_dirty_out);

    // ── Miss classification ───────────────────────────────────────────────────
    AccessResult classify_miss(uint32_t set_idx, uint64_t tag) const;

    CacheConfig               config_;
    std::vector<CacheSet>     sets_;
    CacheStats                stats_;
    uint64_t                  global_time_ = 0;  ///< Logical clock for LRU

    // Track ever-seen tags per set for compulsory miss detection
    std::vector<std::vector<uint64_t>> seen_tags_;

    // Co-sim log buffer
    std::vector<AccessLog>    log_;
};
