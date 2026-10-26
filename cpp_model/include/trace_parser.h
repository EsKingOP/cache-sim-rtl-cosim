#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <optional>

#include "cache.h"  // For AccessType

// ─────────────────────────────────────────────────────────────────────────────
//  Memory access record
// ─────────────────────────────────────────────────────────────────────────────

struct MemAccess {
    uint64_t   address;          ///< Target address (hex in trace file)
    AccessType type;             ///< READ or WRITE
    uint32_t   data  = 0;        ///< Write data (0 for reads)
    bool       has_data = false; ///< True if data field was present in trace

    /// Source line number (for error reporting)
    uint32_t   line_number = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Trace parser
// ─────────────────────────────────────────────────────────────────────────────

class TraceParser {
public:
    /// Parse options
    struct Options {
        bool skip_comments  = true;   ///< Lines starting with '#' are comments
        bool skip_blank     = true;   ///< Skip empty / whitespace-only lines
        std::size_t max_accesses = 0; ///< 0 = unlimited
    };

    explicit TraceParser(const std::string& path,
                         Options opts = {});

    /// Parse entire file into memory. Throws on I/O or parse error.
    std::vector<MemAccess> parse_all();

    /// Parse one access at a time. Returns std::nullopt at EOF.
    std::optional<MemAccess> next();

    /// Rewind to the beginning of the file.
    void rewind();

    /// Number of accesses successfully parsed so far.
    std::size_t parsed_count() const { return parsed_count_; }

    /// Path of the file being parsed.
    const std::string& path() const { return path_; }

private:
    /// Parse a single non-empty, non-comment line into a MemAccess.
    /// Format:  [R|W] 0xADDRESS [0xDATA]
    /// Throws std::runtime_error on malformed line.
    MemAccess parse_line(const std::string& line, uint32_t line_no) const;

    std::string   path_;
    std::ifstream file_;
    Options       opts_;
    std::size_t   parsed_count_ = 0;
    uint32_t      current_line_ = 0;
};
