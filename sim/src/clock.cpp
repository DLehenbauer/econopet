#include "clock.hpp"

CClock::CClock() { }

TimePoint CClock::now() {
    return { std::chrono::high_resolution_clock::now() };
}

uint32_t CClock::elapsed(TimePoint start, TimePoint end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end.value - start.value).count() / 1000;
}
