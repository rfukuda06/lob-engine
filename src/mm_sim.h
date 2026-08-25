#pragma once
#include <string>

#include "analytics.h"
#include "flow.h"
#include "market_maker.h"

namespace lob {

struct MmSimConfig {
    long steps = 20000;
    unsigned seed = 42;
    MMPolicy policy = MMPolicy::InventorySkew;
    FlowParams flow;
    MMParams mm;
    std::string csvPath;   // empty => no CSV
    bool quiet = false;    // suppress summary printing (tests set this)
};

// Runs the market-making simulation and returns the computed summary.
Summary runMarketMakingSim(const MmSimConfig& config);

}  // namespace lob
