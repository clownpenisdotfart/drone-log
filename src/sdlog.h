// CSV detection log on the Cardputer's microSD card.
#pragma once

#include <stdint.h>

#include "drones.h"

namespace sdlog {

// Mounts the card and opens a fresh /remoteid/rid_NNNN.csv. Safe to call
// again later to retry after a missing or removed card.
bool begin();
bool ready();
const char* fileName();
uint32_t rows();

// Appends one row describing `d`. `utc_base_s` is the Unix time at uptime 0,
// or 0 when wall-clock time is not known yet.
void write(const Drone& d, uint32_t now_ms, int64_t utc_base_s);
void flush();

}  // namespace sdlog
