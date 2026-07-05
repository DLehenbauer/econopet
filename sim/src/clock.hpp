#include "pch.hpp"
#pragma once

struct TimePoint {
    std::chrono::high_resolution_clock::time_point value;
};

class CClock {
    public:
        CClock();
        static TimePoint now();
        static uint32_t elapsed(TimePoint start, TimePoint end);
};
