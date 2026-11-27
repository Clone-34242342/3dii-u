#pragma once
#include <cstdint>
using OSTime = int64_t;
OSTime OSGetSystemTime();
inline OSTime OSMillisecondsToTicks(int64_t ms) { return ms; }
