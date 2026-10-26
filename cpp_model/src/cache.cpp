#include "cache.h"

#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: integer log2 for powers of two
// ─────────────────────────────────────────────────────────────────────────────

static int ilog2(std::size_t n) {
    int bits = 0;
    while (n > 1) { n >>= 1; ++bits; }
    return bits;
}

static bool is_power_of_two(std::size_t n) {
    return n && !(n & (n - 1));
}

// ─────────────────────────────────────────────────────────────────────────────
//  CacheConfig
// ─────────────────────────────────────────────────────────────────────────────

int CacheConfig::offset_bits() const {
    return ilog2(line_size);
}

int CacheConfig::index_bits() const {
    return ilog2(num_sets);
}

int CacheConfig::tag_bits() const {
    return 64 - offset_bits() - index_bits();
}

bool CacheConfig::is_valid() const {
    if (!is_power_of_two(line_size))  return false;
    if (!is_power_of_two(num_sets))   return false;
    if (!is_power_of_two(num_ways))   return false;
    if (num_ways == 0 || num_sets == 0 || line_size == 0) return false;
    if (offset_bits() + index_bits() >= 64) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CacheStats::print
// ─────────────────────────────────────────────────────────────────────────────

void CacheStats::print() const {
    auto pct = [&](uint64_t n) -> std::string {
        if (total_accesses == 0) return "  0.00%";
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2)
           << (100.0 * static_cast<double>(n) / static_cast<double>(total_accesses)) << "%";
        return ss.str();
    };

    std::cout << "\nSimulation Results (" << total_accesses << " accesses):\n";
    std::cout << "  Reads:              " << reads  << "\n";
    std::cout << "  Writes:             " << writes << "\n";
    std::cout << "  Hits:               " << hits   << "  (" << pct(hits)   << ")\n";
    std::cout << "  Misses:             " << misses << "  (" << pct(misses) << ")\n";
    std::cout << "    Compulsory:       " << compulsory_misses << "\n";
    std::cout << "    Conflict:         " << conflict_misses  << "\n";
    std::cout << "    Capacity:         " << capacity_misses  << "\n";
    std::cout << "  Evictions:          " << evictions      << "\n";
    std::cout << "    Dirty evictions:  " << dirty_evictions << "\n";
    std::cout << "  Writebacks:         " << writebacks     << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Cache constructor
// ─────────────────────────────────────────────────────────────────────────────

Cache::Cache(const CacheConfig& config)
    : config_(config)
{
    if (!config_.is_valid()) {
        throw std::invalid_argument(
            "CacheConfig is invalid: num_sets, num_ways, and line_size must be "
            "non-zero powers of two, and offset+index bits must be < 64.");
    }

    sets_.reserve(config_.num_sets);
    seen_tags_.resize(config_.num_sets);

    for (std::size_t i = 0; i < config_.num_sets; ++i) {
        sets_.emplace_back(config_.num_ways);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Address decomposition
// ─────────────────────────────────────────────────────────────────────────────

uint64_t Cache::extract_tag(uint64_t addr) const {
    return addr >> (config_.offset_bits() + config_.index_bits());
}

uint32_t Cache::extract_index(uint64_t addr) const {
    const uint64_t mask = (static_cast<uint64_t>(1) << config_.index_bits()) - 1;
    return static_cast<uint32_t>((addr >> config_.offset_bits()) & mask);
}

uint32_t Cache::extract_offset(uint64_t addr) const {
    const uint64_t mask = (static_cast<uint64_t>(1) << config_.offset_bits()) - 1;
    return static_cast<uint32_t>(addr & mask);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hit detection
// ─────────────────────────────────────────────────────────────────────────────

int Cache::find_hit_way(const CacheSet& set, uint64_t tag) const {
    for (std::size_t w = 0; w < config_.num_ways; ++w) {
        const auto& line = set.ways[w];
        if (line.valid && line.tag == tag) {
            return static_cast<int>(w);
        }
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Replacement policy: victim selection
// ─────────────────────────────────────────────────────────────────────────────

int Cache::find_victim(const CacheSet& set) const {
    // First preference: invalid (empty) way
    for (std::size_t w = 0; w < config_.num_ways; ++w) {
        if (!set.ways[w].valid) return static_cast<int>(w);
    }

    switch (config_.policy) {
        case ReplacementPolicy::LRU: {
            // Evict the way with the smallest last_used_time
            int    victim   = 0;
            uint64_t oldest = set.ways[0].last_used_time;
            for (std::size_t w = 1; w < config_.num_ways; ++w) {
                if (set.ways[w].last_used_time < oldest) {
                    oldest = set.ways[w].last_used_time;
                    victim = static_cast<int>(w);
                }
            }
            return victim;
        }
        case ReplacementPolicy::FIFO: {
            // fifo_head points to the oldest entry
            return static_cast<int>(set.fifo_head % config_.num_ways);
        }
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  LRU timestamp update
// ─────────────────────────────────────────────────────────────────────────────

void Cache::update_lru(CacheSet& set, int used_way) {
    set.ways[static_cast<std::size_t>(used_way)].last_used_time = global_time_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Insert new line, possibly evicting victim
// ─────────────────────────────────────────────────────────────────────────────

int Cache::insert_line(CacheSet& set, uint64_t tag, bool dirty,
                       int* evict_way_out, bool* evict_dirty_out) {
    const int victim = find_victim(set);
    auto& line = set.ways[static_cast<std::size_t>(victim)];

    *evict_way_out   = -1;
    *evict_dirty_out = false;

    if (line.valid) {
        // Eviction
        *evict_way_out   = victim;
        *evict_dirty_out = line.dirty;

        ++stats_.evictions;
        if (line.dirty) {
            ++stats_.dirty_evictions;
            ++stats_.writebacks;
        }
    }

    // Install new line
    line.valid          = true;
    line.dirty          = dirty;
    line.tag            = tag;
    line.last_used_time = global_time_;

    if (config_.policy == ReplacementPolicy::FIFO) {
        line.fifo_order = set.fifo_head;
        // Advance FIFO pointer (wraps modulo num_ways)
        set.fifo_head = static_cast<uint32_t>(
            (set.fifo_head + 1) % config_.num_ways);
    }

    return victim;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Miss classification
// ─────────────────────────────────────────────────────────────────────────────

AccessResult Cache::classify_miss(uint32_t set_idx, uint64_t tag) const {
    const auto& tags = seen_tags_[set_idx];
    bool ever_seen = std::find(tags.begin(), tags.end(), tag) != tags.end();

    if (!ever_seen) {
        return AccessResult::MISS_COMPULSORY;
    }

    // If the set is fully valid (all ways occupied), it's a conflict or capacity miss.
    // We use a simple heuristic: if the set has been fully populated at any point,
    // classify as conflict; else capacity.
    bool set_full = true;
    for (const auto& line : sets_[set_idx].ways) {
        if (!line.valid) { set_full = false; break; }
    }
    return set_full ? AccessResult::MISS_CONFLICT : AccessResult::MISS_CAPACITY;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Core access (logged)
// ─────────────────────────────────────────────────────────────────────────────

AccessLog Cache::access_logged(uint64_t address, AccessType type, uint32_t /*data*/) {
    ++global_time_;
    ++stats_.total_accesses;

    if (type == AccessType::READ)  ++stats_.reads;
    else                           ++stats_.writes;

    const uint64_t tag       = extract_tag(address);
    const uint32_t set_idx   = extract_index(address);

    CacheSet& set = sets_[set_idx];
    AccessLog log_entry{};
    log_entry.index      = stats_.total_accesses;
    log_entry.address    = address;
    log_entry.type       = type;
    log_entry.tag        = tag;
    log_entry.set_index  = set_idx;
    log_entry.way_hit    = -1;
    log_entry.way_evict  = -1;
    log_entry.evicted_dirty = false;

    const int hit_way = find_hit_way(set, tag);

    if (hit_way >= 0) {
        // ── HIT ──────────────────────────────────────────────────────────────
        ++stats_.hits;

        if (type == AccessType::WRITE) {
            set.ways[static_cast<std::size_t>(hit_way)].dirty = true;
        }
        update_lru(set, hit_way);

        log_entry.result  = AccessResult::HIT;
        log_entry.way_hit = hit_way;
    } else {
        // ── MISS ─────────────────────────────────────────────────────────────
        ++stats_.misses;

        // Record tag as seen (before classification)
        seen_tags_[set_idx].push_back(tag);

        const AccessResult miss_type = classify_miss(set_idx, tag);
        log_entry.result = miss_type;

        switch (miss_type) {
            case AccessResult::MISS_COMPULSORY: ++stats_.compulsory_misses; break;
            case AccessResult::MISS_CONFLICT:   ++stats_.conflict_misses;   break;
            case AccessResult::MISS_CAPACITY:   ++stats_.capacity_misses;   break;
            default: break;
        }

        // Write-allocate for writes; fetch for reads
        bool dirty_new = (type == AccessType::WRITE &&
                          config_.write_policy == WritePolicy::WRITE_BACK);

        int  evict_way   = -1;
        bool evict_dirty = false;
        const int new_way = insert_line(set, tag, dirty_new,
                                        &evict_way, &evict_dirty);

        log_entry.way_evict    = evict_way;
        log_entry.evicted_dirty = evict_dirty;

        // LRU: just inserted — mark as most recently used
        update_lru(set, new_way);
    }

    log_.push_back(log_entry);
    return log_entry;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Simpler access (no log)
// ─────────────────────────────────────────────────────────────────────────────

AccessResult Cache::access(uint64_t address, AccessType type, uint32_t data) {
    return access_logged(address, type, data).result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Flush
// ─────────────────────────────────────────────────────────────────────────────

std::size_t Cache::flush() {
    std::size_t wb_count = 0;
    for (auto& set : sets_) {
        for (auto& line : set.ways) {
            if (line.valid && line.dirty) {
                line.dirty = false;
                ++wb_count;
                ++stats_.writebacks;
            }
        }
    }
    return wb_count;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Reset
// ─────────────────────────────────────────────────────────────────────────────

void Cache::reset() {
    for (auto& set : sets_) {
        for (auto& line : set.ways) {
            line = CacheLine{};
        }
        set.fifo_head = 0;
    }
    stats_      = CacheStats{};
    global_time_ = 0;
    log_.clear();
    for (auto& sv : seen_tags_) sv.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Write log
// ─────────────────────────────────────────────────────────────────────────────

void Cache::write_log(const std::string& path) const {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot open log file: " + path);

    auto result_str = [](AccessResult r) -> const char* {
        switch (r) {
            case AccessResult::HIT:              return "HIT";
            case AccessResult::MISS_COMPULSORY:  return "MISS_COMPULSORY";
            case AccessResult::MISS_CONFLICT:    return "MISS_CONFLICT";
            case AccessResult::MISS_CAPACITY:    return "MISS_CAPACITY";
        }
        return "UNKNOWN";
    };

    for (const auto& e : log_) {
        f << e.index << " "
          << (e.type == AccessType::READ ? "R" : "W") << " "
          << "0x" << std::hex << std::setw(8) << std::setfill('0') << e.address << std::dec << " "
          << result_str(e.result) << " "
          << "SET=" << e.set_index << " "
          << "WAY_HIT=" << e.way_hit << " "
          << "WAY_EVICT=" << e.way_evict << " "
          << "DIRTY_EVICT=" << (e.evicted_dirty ? "1" : "0")
          << "\n";
    }
}
