#pragma once
#include <cstdint>

struct Config {
    bool box;
    bool skeleton;
    bool health;
    bool name;
    bool teamcheck;
};

extern Config g_cfg;
