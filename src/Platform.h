// The few things that differ between systems, or that tests replace: the
// clock, where the zone database lives, and the machine's own time zone.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace timemcp {

// Seconds since 1970-01-01 UTC. Every tool reads the clock through this one
// function. TIME_MCP_TEST_NOW=<seconds> in the environment replaces the
// clock with a fixed instant, for tests.
int64_t currentTime();

// The zone database directory: TZDIR from the environment when it is set,
// else the first standard location that exists.
const std::string &zoneDatabaseDirectory();

// Names the machine's time zone might have, most trusted first. The caller
// keeps the first one that loads.
std::vector<std::string> localZoneCandidates();

} // namespace timemcp
