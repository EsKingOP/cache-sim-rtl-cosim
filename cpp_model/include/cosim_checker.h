#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <ostream>

// ─────────────────────────────────────────────────────────────────────────────
//  Mismatch categories
// ─────────────────────────────────────────────────────────────────────────────

enum class MismatchCategory {
    WRITE_BUFFER_SYNC,      ///< RTL write buffer not flushed before same-line read
    LRU_UPDATE_DELAY,       ///< RTL LRU state updated one cycle late
    EVICT_BEFORE_FILL,      ///< RTL evicted line before fill completed
    DIRTY_BIT_CLEAR,        ///< RTL dirty bit not cleared after writeback
    FIFO_POINTER_WRAP,      ///< FIFO victim pointer off-by-one at wrap-around
    PARTIAL_WRITE_MASK,     ///< Byte-enable mask not applied to dirty tracking
    COMPULSORY_VS_CONFLICT, ///< Miss-type classification differs (stats only)
    RESET_STATE_MISMATCH,   ///< Post-reset LRU/FIFO state divergence
    UNKNOWN,                ///< Unclassified mismatch
};

std::string to_string(MismatchCategory cat);

// ─────────────────────────────────────────────────────────────────────────────
//  RTL log entry (parsed from RTL simulation output log)
// ─────────────────────────────────────────────────────────────────────────────

struct RtlLogEntry {
    uint64_t    index;          ///< Access sequence number (must match cpp log)
    uint64_t    address;
    char        access_type;    ///< 'R' or 'W'
    std::string result;         ///< "HIT", "MISS", "EVICT_DIRTY", "EVICT_CLEAN"
    uint32_t    set_index;
    int         way_hit;        ///< -1 on miss
    int         way_evict;      ///< -1 if no eviction
};

// ─────────────────────────────────────────────────────────────────────────────
//  A single mismatch record
// ─────────────────────────────────────────────────────────────────────────────

struct Mismatch {
    uint64_t          access_index;
    uint64_t          address;
    char              access_type;
    std::string       cpp_result;
    std::string       rtl_result;
    MismatchCategory  category;
    std::string       detail;    ///< Human-readable description
};

// ─────────────────────────────────────────────────────────────────────────────
//  Co-sim checker
// ─────────────────────────────────────────────────────────────────────────────

class CosimChecker {
public:
    struct Config {
        bool        verbose        = false;  ///< Print every comparison
        bool        stop_on_first  = false;  ///< Stop after first mismatch
        std::size_t max_mismatches = 100;    ///< Limit mismatch list size
    };

    explicit CosimChecker(Config cfg = {});

    /// Load the C++ model's per-access log file.
    void load_cpp_log(const std::string& path);

    /// Load the RTL simulator's output log file.
    void load_rtl_log(const std::string& path);

    /// Run the comparison. Returns true if no mismatches.
    bool compare();

    /// All mismatches found (populated after compare()).
    const std::vector<Mismatch>& mismatches() const { return mismatches_; }

    /// Print a formatted report to the given stream.
    void print_report(std::ostream& out) const;

    /// Summary statistics by category.
    struct CategorySummary {
        MismatchCategory category;
        std::size_t      count;
    };
    std::vector<CategorySummary> category_summary() const;

private:
    // ── Parsers ───────────────────────────────────────────────────────────────
    struct CppLogEntry {
        uint64_t    index;
        uint64_t    address;
        char        access_type;
        std::string result;
        uint32_t    set_index;
        int         way_hit;
        int         way_evict;
        bool        evicted_dirty;
    };

    std::vector<CppLogEntry> parse_cpp_log(const std::string& path);
    std::vector<RtlLogEntry> parse_rtl_log(const std::string& path);

    /// Classify a mismatch based on the C++ and RTL entries.
    MismatchCategory classify(const CppLogEntry& cpp,
                              const RtlLogEntry& rtl) const;

    Config                   cfg_;
    std::vector<CppLogEntry> cpp_log_;
    std::vector<RtlLogEntry> rtl_log_;
    std::vector<Mismatch>    mismatches_;
};
