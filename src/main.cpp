#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "benchmark.h"
#include "matching_engine.h"
#include "mm_sim.h"
#include "repl.h"
#include "simulator.h"

namespace {

void printUsage() {
    std::printf(
        "usage: orderbook [--benchmark [N]] [--mm-sim [N]] [--seed S]\n"
        "                 [--informed-frac F] [--policy inventory|as] [--out FILE]\n"
        "  (no args)         interactive REPL with a simulated market\n"
        "  --benchmark [N]   process N generated orders (default 1000000)\n"
        "  --mm-sim [N]      run the market-making lab for N steps (default 20000)\n"
        "  --seed S          RNG seed (default 42)\n"
        "  --informed-frac F fraction of informed flow, 0..100 percent (default 15)\n"
        "  --policy P        market-maker policy: inventory (default) or as\n"
        "  --out FILE        write the per-step CSV to FILE\n");
}

// Returns nullopt on parse failure.
std::optional<long long> parseNumber(const char* text) {
    std::string s(text);
    if (s.empty() || s.size() > 12) return std::nullopt;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    return std::stoll(s);
}

}  // namespace

int main(int argc, char** argv) {
    bool benchmark = false;
    std::size_t benchmarkOrders = 1000000;
    unsigned seed = lob::MarketSimulator::kDefaultSeed;
    bool mmSim = false;
    lob::MmSimConfig mmCfg;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--benchmark") == 0) {
            benchmark = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                auto n = parseNumber(argv[++i]);
                if (!n || *n <= 0) { printUsage(); return 1; }
                benchmarkOrders = static_cast<std::size_t>(*n);
            }
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            auto s = parseNumber(argv[++i]);
            if (!s) { printUsage(); return 1; }
            seed = static_cast<unsigned>(*s);
        } else if (std::strcmp(argv[i], "--mm-sim") == 0) {
            mmSim = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                auto n = parseNumber(argv[++i]);
                if (!n || *n <= 0) { printUsage(); return 1; }
                mmCfg.steps = static_cast<long>(*n);
            }
        } else if (std::strcmp(argv[i], "--informed-frac") == 0 && i + 1 < argc) {
            auto v = parseNumber(argv[++i]);
            if (!v || *v > 100) { printUsage(); return 1; }
            mmCfg.flow.informedFraction = static_cast<double>(*v) / 100.0;
        } else if (std::strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            std::string pol = argv[++i];
            if (pol == "inventory") mmCfg.policy = lob::MMPolicy::InventorySkew;
            else if (pol == "as") mmCfg.policy = lob::MMPolicy::AvellanedaStoikov;
            else { printUsage(); return 1; }
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            mmCfg.csvPath = argv[++i];
        } else {
            printUsage();
            return std::strcmp(argv[i], "--help") == 0 ? 0 : 1;
        }
    }

    if (mmSim) {
        mmCfg.seed = seed;
        lob::runMarketMakingSim(mmCfg);
        return 0;
    }

    if (benchmark) {
        lob::runBenchmark(benchmarkOrders, seed);
        return 0;
    }

    lob::MatchingEngine engine;
    lob::MarketSimulator sim(engine, seed);
    sim.seedInitialLiquidity();
    lob::runRepl(engine, sim);
    return 0;
}
