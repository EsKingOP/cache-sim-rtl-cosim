#include "cosim_checker.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>

// ─────────────────────────────────────────────────────────────────────────────
//  to_string(MismatchCategory)
// ─────────────────────────────────────────────────────────────────────────────

std::string to_string(MismatchCategory cat) {
    switch (cat) {
        case MismatchCategory::WRITE_BUFFER_SYNC:      return "WRITE_BUFFER_SYNC";
        case MismatchCategory::LRU_UPDATE_DELAY:       return "LRU_UPDATE_DELAY";
        case MismatchCategory::EVICT_BEFORE_FILL:      return "EVICT_BEFORE_FILL";
        case MismatchCategory::DIRTY_BIT_CLEAR:        return "DIRTY_BIT_CLEAR";
        case MismatchCategory::FIFO_POINTER_WRAP:      return "FIFO_POINTER_WRAP";
        case MismatchCategory::PARTIAL_WRITE_MASK:     return "PARTIAL_WRITE_MASK";
        case MismatchCategory::COMPULSORY_VS_CONFLICT: return "COMPULSORY_VS_CONFLICT";
        case MismatchCategory::RESET_STATE_MISMATCH:   return "RESET_STATE_MISMATCH";
        case MismatchCategory::UNKNOWN:                return "UNKNOWN";
    }
    return "UNKNOWN";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────

CosimChecker::CosimChecker(Config cfg)
    : cfg_(cfg) {}

// ─────────────────────────────────────────────────────────────────────────────
//  Load logs
// ─────────────────────────────────────────────────────────────────────────────

void CosimChecker::load_cpp_log(const std::string& path) {
    cpp_log_ = parse_cpp_log(path);
}

void CosimChecker::load_rtl_log(const std::string& path) {
    rtl_log_ = parse_rtl_log(path);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parse C++ model log
//
//  Format (from Cache::write_log):
//    <idx> <R|W> 0x<addr> <RESULT> SET=<n> WAY_HIT=<n> WAY_EVICT=<n> DIRTY_EVICT=<0|1>
// ─────────────────────────────────────────────────────────────────────────────

std::vector<CosimChecker::CppLogEntry>
CosimChecker::parse_cpp_log(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("CosimChecker: cannot open cpp log: " + path);

    std::vector<CppLogEntry> entries;
    std::string line;
    uint32_t line_no = 0;

    while (std::getline(f, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        CppLogEntry e{};
        std::string addr_str, result_str, set_str, way_hit_str, way_evict_str, dirty_str;

        if (!(ss >> e.index >> e.access_type >> addr_str >> result_str
                 >> set_str >> way_hit_str >> way_evict_str >> dirty_str)) {
            throw std::runtime_error(
                path + ":" + std::to_string(line_no) +
                ": malformed cpp log line");
        }

        // Parse address
        const char* ap = addr_str.c_str();
        if (addr_str.size() >= 2 && ap[0] == '0' && (ap[1] == 'x' || ap[1] == 'X'))
            ap += 2;
        std::from_chars(ap, ap + addr_str.size(), e.address, 16);

        e.result = result_str;

        // Parse SET=N
        {
            auto eq = set_str.find('=');
            if (eq != std::string::npos)
                e.set_index = static_cast<uint32_t>(
                    std::stoul(set_str.substr(eq + 1)));
        }
        // Parse WAY_HIT=N
        {
            auto eq = way_hit_str.find('=');
            if (eq != std::string::npos)
                e.way_hit = std::stoi(way_hit_str.substr(eq + 1));
        }
        // Parse WAY_EVICT=N
        {
            auto eq = way_evict_str.find('=');
            if (eq != std::string::npos)
                e.way_evict = std::stoi(way_evict_str.substr(eq + 1));
        }
        // Parse DIRTY_EVICT=N
        {
            auto eq = dirty_str.find('=');
            if (eq != std::string::npos)
                e.evicted_dirty = (dirty_str.substr(eq + 1) == "1");
        }

        entries.push_back(e);
    }

    return entries;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parse RTL log
//
//  Format written by tb_cache_controller.v:
//    <idx> <R|W> 0x<addr> <HIT|MISS|EVICT_DIRTY|EVICT_CLEAN> SET=<n> WAY_HIT=<n> WAY_EVICT=<n>
// ─────────────────────────────────────────────────────────────────────────────

std::vector<RtlLogEntry>
CosimChecker::parse_rtl_log(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("CosimChecker: cannot open rtl log: " + path);

    std::vector<RtlLogEntry> entries;
    std::string line;
    uint32_t line_no = 0;

    while (std::getline(f, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        RtlLogEntry e{};
        std::string addr_str, result_str, set_str, way_hit_str, way_evict_str;

        if (!(ss >> e.index >> e.access_type >> addr_str >> result_str
                 >> set_str >> way_hit_str >> way_evict_str)) {
            throw std::runtime_error(
                path + ":" + std::to_string(line_no) +
                ": malformed rtl log line");
        }

        const char* ap = addr_str.c_str();
        if (addr_str.size() >= 2 && ap[0] == '0' && (ap[1] == 'x' || ap[1] == 'X'))
            ap += 2;
        std::from_chars(ap, ap + addr_str.size(), e.address, 16);

        e.result = result_str;

        auto parse_kv = [](const std::string& s) -> int {
            auto eq = s.find('=');
            if (eq != std::string::npos)
                return std::stoi(s.substr(eq + 1));
            return -1;
        };

        e.set_index  = static_cast<uint32_t>(parse_kv(set_str));
        e.way_hit    = parse_kv(way_hit_str);
        e.way_evict  = parse_kv(way_evict_str);

        entries.push_back(e);
    }

    return entries;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Classify a single mismatch
// ─────────────────────────────────────────────────────────────────────────────

MismatchCategory CosimChecker::classify(const CppLogEntry& cpp,
                                         const RtlLogEntry& rtl) const {
    const bool cpp_hit  = (cpp.result == "HIT");
    const bool rtl_hit  = (rtl.result == "HIT");
    const bool cpp_miss = !cpp_hit;
    const bool rtl_miss = !rtl_hit;

    // RTL reports miss but C++ says hit — classic write-buffer sync issue
    if (cpp_hit && rtl_miss && cpp.access_type == 'R') {
        return MismatchCategory::WRITE_BUFFER_SYNC;
    }

    // Both agree on hit/miss but differ on evicted way — LRU update timing
    if (cpp_hit && rtl_hit && cpp.way_hit != rtl.way_hit) {
        return MismatchCategory::LRU_UPDATE_DELAY;
    }

    // RTL evicts before fill completes: RTL shows EVICT before MISS fill done
    if (rtl.result == "EVICT_DIRTY" && cpp_miss && cpp.way_evict == -1) {
        return MismatchCategory::EVICT_BEFORE_FILL;
    }

    // RTL still marks dirty after writeback
    if (cpp_miss && !cpp.evicted_dirty && rtl.result == "EVICT_DIRTY") {
        return MismatchCategory::DIRTY_BIT_CLEAR;
    }

    // FIFO pointer wrap: eviction victim differs at set boundary
    if (cpp_miss && rtl_miss && cpp.way_evict != rtl.way_evict) {
        // Could be FIFO wrap or LRU delay — distinguish by access type history
        if (cpp.access_type == 'W') {
            return MismatchCategory::FIFO_POINTER_WRAP;
        }
        return MismatchCategory::LRU_UPDATE_DELAY;
    }

    // Partial write mask: write hit result differs
    if (cpp.access_type == 'W' && cpp_hit && rtl_miss) {
        return MismatchCategory::PARTIAL_WRITE_MASK;
    }

    // Miss type classification only (stats divergence, not functional)
    if (cpp_miss && rtl_miss && cpp.result != rtl.result) {
        return MismatchCategory::COMPULSORY_VS_CONFLICT;
    }

    // Catch-all (likely reset-state divergence early in trace)
    (void)rtl_miss;
    return MismatchCategory::RESET_STATE_MISMATCH;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Compare
// ─────────────────────────────────────────────────────────────────────────────

bool CosimChecker::compare() {
    mismatches_.clear();

    const std::size_t n = std::min(cpp_log_.size(), rtl_log_.size());

    if (cpp_log_.size() != rtl_log_.size()) {
        std::cerr << "[COSIM] WARNING: cpp log has " << cpp_log_.size()
                  << " entries, rtl log has " << rtl_log_.size()
                  << " entries; comparing first " << n << "\n";
    }

    for (std::size_t i = 0; i < n; ++i) {
        const auto& cpp = cpp_log_[i];
        const auto& rtl = rtl_log_[i];

        if (cfg_.verbose) {
            std::cout << "[COSIM] #" << cpp.index
                      << " CPP=" << cpp.result
                      << " RTL=" << rtl.result << "\n";
        }

        // Normalise: MISS_COMPULSORY / MISS_CONFLICT / MISS_CAPACITY all map to "MISS"
        auto normalize = [](const std::string& r) -> std::string {
            if (r == "MISS_COMPULSORY" || r == "MISS_CONFLICT" || r == "MISS_CAPACITY")
                return "MISS";
            return r;
        };

        // For functional comparison: HIT vs MISS is the key signal
        bool match = (normalize(cpp.result) == normalize(rtl.result));

        // Additionally check eviction dirtiness agreement
        bool dirty_mismatch = (cpp.evicted_dirty != (rtl.result == "EVICT_DIRTY"));
        if (normalize(cpp.result) == "MISS" && normalize(rtl.result) == "MISS") {
            if (dirty_mismatch) match = false;
        }

        if (!match) {
            Mismatch mm{};
            mm.access_index = cpp.index;
            mm.address      = cpp.address;
            mm.access_type  = cpp.access_type;
            mm.cpp_result   = cpp.result;
            mm.rtl_result   = rtl.result;
            mm.category     = classify(cpp, rtl);

            std::ostringstream detail;
            detail << "CPP=" << cpp.result << " RTL=" << rtl.result
                   << " SET=" << cpp.set_index
                   << " CPP_WAY_HIT=" << cpp.way_hit
                   << " RTL_WAY_HIT=" << rtl.way_hit;
            mm.detail = detail.str();

            mismatches_.push_back(mm);

            if (cfg_.stop_on_first) break;
            if (mismatches_.size() >= cfg_.max_mismatches) break;
        }
    }

    return mismatches_.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
//  print_report
// ─────────────────────────────────────────────────────────────────────────────

void CosimChecker::print_report(std::ostream& out) const {
    const std::size_t total = std::max(cpp_log_.size(), rtl_log_.size());

    out << "\n[COSIM] Comparing " << total << " accesses...\n";

    if (mismatches_.empty()) {
        out << "[COSIM] PASS — all accesses match between C++ model and RTL.\n";
        return;
    }

    out << "[COSIM] FAIL — " << mismatches_.size() << " mismatches found.\n\n";

    for (const auto& mm : mismatches_) {
        out << "  MISMATCH #" << mm.access_index << ":\n"
            << "    Address:    0x" << std::hex << std::setw(8) << std::setfill('0')
                                    << mm.address << std::dec << "\n"
            << "    Type:       " << mm.access_type << "\n"
            << "    C++ result: " << mm.cpp_result << "\n"
            << "    RTL result: " << mm.rtl_result << "\n"
            << "    Category:   " << to_string(mm.category) << "\n"
            << "    Detail:     " << mm.detail << "\n\n";
    }

    // Category summary
    auto summary = category_summary();
    out << "  Mismatch summary by category:\n";
    for (const auto& [cat, count] : summary) {
        out << "    " << std::setw(30) << std::left << to_string(cat)
            << count << "\n";
    }
    out << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  category_summary
// ─────────────────────────────────────────────────────────────────────────────

std::vector<CosimChecker::CategorySummary> CosimChecker::category_summary() const {
    std::map<MismatchCategory, std::size_t> counts;
    for (const auto& mm : mismatches_) {
        counts[mm.category]++;
    }
    std::vector<CategorySummary> result;
    for (const auto& [cat, cnt] : counts) {
        result.push_back({cat, cnt});
    }
    std::sort(result.begin(), result.end(),
              [](const CategorySummary& a, const CategorySummary& b){
                  return a.count > b.count;
              });
    return result;
}
