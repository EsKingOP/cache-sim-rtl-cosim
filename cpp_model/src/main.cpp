#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <iomanip>

#include "cache.h"
#include "trace_parser.h"
#include "cosim_checker.h"

// ─────────────────────────────────────────────────────────────────────────────
//  CLI argument parsing (minimal, no external deps)
// ─────────────────────────────────────────────────────────────────────────────

struct Args {
    std::string trace_file;
    std::size_t num_sets    = 64;
    std::size_t num_ways    = 4;
    std::size_t line_size   = 64;
    ReplacementPolicy policy = ReplacementPolicy::LRU;
    std::string write_log_file;         // --write-log <path>
    std::string rtl_log_file;           // --rtl-log <path>  (triggers co-sim check)
    bool        cosim_verbose = false;
    bool        help          = false;
};

static void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [OPTIONS]\n\n"
        << "Options:\n"
        << "  --trace <file>       Memory trace file (required)\n"
        << "  --sets <N>           Number of cache sets (default: 64)\n"
        << "  --ways <N>           Associativity / ways (default: 4)\n"
        << "  --line-size <N>      Cache line size in bytes (default: 64)\n"
        << "  --policy lru|fifo    Replacement policy (default: lru)\n"
        << "  --write-log <file>   Write per-access log for co-sim checker\n"
        << "  --rtl-log <file>     RTL output log; runs co-sim checker after simulation\n"
        << "  --cosim-verbose      Print every comparison in co-sim mode\n"
        << "  --help               Show this message\n";
}

static Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        auto require_next = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(flag + " requires an argument");
            }
            return argv[++i];
        };

        if      (a == "--help")          { args.help = true; }
        else if (a == "--trace")         { args.trace_file = require_next(a); }
        else if (a == "--sets")          { args.num_sets   = std::stoul(require_next(a)); }
        else if (a == "--ways")          { args.num_ways   = std::stoul(require_next(a)); }
        else if (a == "--line-size")     { args.line_size  = std::stoul(require_next(a)); }
        else if (a == "--write-log")     { args.write_log_file = require_next(a); }
        else if (a == "--rtl-log")       { args.rtl_log_file   = require_next(a); }
        else if (a == "--cosim-verbose") { args.cosim_verbose = true; }
        else if (a == "--policy") {
            std::string pol = require_next(a);
            if (pol == "lru")       args.policy = ReplacementPolicy::LRU;
            else if (pol == "fifo") args.policy = ReplacementPolicy::FIFO;
            else throw std::invalid_argument("Unknown policy: " + pol);
        }
        else {
            throw std::invalid_argument("Unknown argument: " + a);
        }
    }
    return args;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Print cache configuration
// ─────────────────────────────────────────────────────────────────────────────

static void print_config(const CacheConfig& cfg) {
    auto pol_name = [](ReplacementPolicy p) -> const char* {
        return p == ReplacementPolicy::LRU ? "LRU" : "FIFO";
    };

    std::size_t cap = cfg.capacity_bytes();
    std::string cap_str;
    if (cap >= 1024 * 1024)
        cap_str = std::to_string(cap / (1024*1024)) + " MB";
    else if (cap >= 1024)
        cap_str = std::to_string(cap / 1024) + " KB";
    else
        cap_str = std::to_string(cap) + " B";

    std::cout << "\nCache Configuration:\n"
              << "  Sets:         " << cfg.num_sets << "\n"
              << "  Ways:         " << cfg.num_ways << "\n"
              << "  Line size:    " << cfg.line_size << " bytes\n"
              << "  Total size:   " << cap_str << "\n"
              << "  Policy:       " << pol_name(cfg.policy)
                                    << ", Write-Back/Write-Allocate\n"
              << "  Offset bits:  " << cfg.offset_bits() << "\n"
              << "  Index bits:   " << cfg.index_bits()  << "\n"
              << "  Tag bits:     " << cfg.tag_bits()    << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (args.help) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (args.trace_file.empty()) {
        std::cerr << "Error: --trace <file> is required.\n";
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // ── Build config ──────────────────────────────────────────────────────────
    CacheConfig cfg;
    cfg.num_sets   = args.num_sets;
    cfg.num_ways   = args.num_ways;
    cfg.line_size  = args.line_size;
    cfg.policy     = args.policy;
    cfg.write_policy = WritePolicy::WRITE_BACK;

    if (!cfg.is_valid()) {
        std::cerr << "Error: invalid cache configuration. "
                     "Sets, ways, and line-size must be non-zero powers of two.\n";
        return EXIT_FAILURE;
    }

    print_config(cfg);

    // ── Parse trace ───────────────────────────────────────────────────────────
    std::vector<MemAccess> trace;
    try {
        TraceParser parser(args.trace_file);
        trace = parser.parse_all();
    } catch (const std::exception& e) {
        std::cerr << "Error parsing trace: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nLoaded " << trace.size() << " accesses from '"
              << args.trace_file << "'\n";

    // ── Run simulation ────────────────────────────────────────────────────────
    Cache cache(cfg);

    for (const auto& acc : trace) {
        cache.access_logged(acc.address, acc.type, acc.data);
    }

    cache.stats().print();

    // ── Write per-access log (for co-sim) ─────────────────────────────────────
    if (!args.write_log_file.empty()) {
        try {
            cache.write_log(args.write_log_file);
            std::cout << "\nPer-access log written to: " << args.write_log_file << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Warning: could not write log: " << e.what() << "\n";
        }
    }

    // ── Co-sim checker ────────────────────────────────────────────────────────
    if (!args.rtl_log_file.empty()) {
        if (args.write_log_file.empty()) {
            std::cerr << "Error: --rtl-log requires --write-log to generate cpp log first.\n";
            return EXIT_FAILURE;
        }

        CosimChecker::Config cosim_cfg;
        cosim_cfg.verbose = args.cosim_verbose;

        CosimChecker checker(cosim_cfg);
        try {
            checker.load_cpp_log(args.write_log_file);
            checker.load_rtl_log(args.rtl_log_file);
        } catch (const std::exception& e) {
            std::cerr << "Error loading logs for co-sim: " << e.what() << "\n";
            return EXIT_FAILURE;
        }

        bool pass = checker.compare();
        checker.print_report(std::cout);

        return pass ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
