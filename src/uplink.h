// Batched upload of drone tracks from fixed nodes to the collector server.
//
// The node's radio never transmits to drones. To upload, WiFi capture is
// paused, the node joins the configured network, POSTs the aircraft tracks
// heard since the last upload over HTTPS, disconnects, and resumes capture.
// Only aircraft position data is uploaded; operator (pilot) location stays on
// the node's SD card.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "drones.h"

namespace uplink {

bool enabled();

// Buffers one aircraft position for the next upload.
void queue(const Drone& d, uint32_t now_ms);

// Uploads when the interval has elapsed. Blocks for a few seconds while it
// does; returns true if an upload was attempted.
bool tick(uint32_t now_ms);

uint32_t sent();
uint32_t failed();
size_t pending();

}  // namespace uplink
