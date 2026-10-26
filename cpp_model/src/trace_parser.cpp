#include "trace_parser.h"

#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <charconv>

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────

TraceParser::TraceParser(const std::string& path, Options opts)
    : path_(path), opts_(opts)
{
    file_.open(path);
    if (!file_.is_open()) {
        throw std::runtime_error("TraceParser: cannot open file '" + path + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Rewind
// ─────────────────────────────────────────────────────────────────────────────

void TraceParser::rewind() {
    file_.clear();
    file_.seekg(0, std::ios::beg);
    parsed_count_ = 0;
    current_line_ = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parse all
// ─────────────────────────────────────────────────────────────────────────────

std::vector<MemAccess> TraceParser::parse_all() {
    std::vector<MemAccess> accesses;
    while (true) {
        auto ma = next();
        if (!ma.has_value()) break;
        accesses.push_back(std::move(*ma));
    }
    return accesses;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Next access
// ─────────────────────────────────────────────────────────────────────────────

std::optional<MemAccess> TraceParser::next() {
    if (opts_.max_accesses > 0 && parsed_count_ >= opts_.max_accesses) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(file_, line)) {
        ++current_line_;

        // Trim leading whitespace
        auto first = std::find_if(line.begin(), line.end(),
                                  [](unsigned char c){ return !std::isspace(c); });
        if (first != line.begin()) {
            line.erase(line.begin(), first);
        }

        // Skip blank lines
        if (opts_.skip_blank && line.empty()) continue;

        // Skip comments
        if (opts_.skip_comments && !line.empty() && line[0] == '#') continue;

        // Remove inline comments (# to end of line)
        auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }

        // Trim trailing whitespace
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }

        if (line.empty()) continue;

        MemAccess ma = parse_line(line, current_line_);
        ma.line_number = current_line_;
        ++parsed_count_;
        return ma;
    }

    return std::nullopt;  // EOF
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parse a single trace line
//
//  Format:
//    R 0xADDR
//    W 0xADDR 0xDATA
//
//  Addresses and data are hex (with or without 0x prefix).
// ─────────────────────────────────────────────────────────────────────────────

MemAccess TraceParser::parse_line(const std::string& line, uint32_t line_no) const {
    std::istringstream ss(line);
    std::string type_tok, addr_tok;

    if (!(ss >> type_tok >> addr_tok)) {
        throw std::runtime_error(
            path_ + ":" + std::to_string(line_no) +
            ": malformed trace line: '" + line + "'");
    }

    // ── Access type ──────────────────────────────────────────────────────────
    AccessType atype;
    if (type_tok == "R" || type_tok == "r") {
        atype = AccessType::READ;
    } else if (type_tok == "W" || type_tok == "w") {
        atype = AccessType::WRITE;
    } else {
        throw std::runtime_error(
            path_ + ":" + std::to_string(line_no) +
            ": unknown access type '" + type_tok +
            "', expected R or W");
    }

    // ── Address ──────────────────────────────────────────────────────────────
    uint64_t address = 0;
    {
        const char* str = addr_tok.c_str();
        const char* end = str + addr_tok.size();
        if (addr_tok.size() >= 2 && str[0] == '0' &&
            (str[1] == 'x' || str[1] == 'X')) {
            str += 2;
        }
        auto [ptr, ec] = std::from_chars(str, end, address, 16);
        if (ec != std::errc{} || ptr != end) {
            throw std::runtime_error(
                path_ + ":" + std::to_string(line_no) +
                ": invalid address '" + addr_tok + "'");
        }
    }

    // ── Optional data ────────────────────────────────────────────────────────
    MemAccess ma;
    ma.address   = address;
    ma.type      = atype;
    ma.has_data  = false;
    ma.data      = 0;

    std::string data_tok;
    if (ss >> data_tok) {
        uint32_t data = 0;
        const char* str = data_tok.c_str();
        const char* end = str + data_tok.size();
        if (data_tok.size() >= 2 && str[0] == '0' &&
            (str[1] == 'x' || str[1] == 'X')) {
            str += 2;
        }
        auto [ptr, ec] = std::from_chars(str, end, data, 16);
        if (ec != std::errc{} || ptr != end) {
            throw std::runtime_error(
                path_ + ":" + std::to_string(line_no) +
                ": invalid data value '" + data_tok + "'");
        }
        ma.data     = data;
        ma.has_data = true;
    }

    return ma;
}
