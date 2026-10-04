// Passive Remote ID receivers: WiFi promiscuous capture (beacon + NAN) and
// BLE passive scanning. Nothing here ever transmits: WiFi is never connected
// or scanned, and BLE scanning is passive (no scan requests are sent).
#pragma once

#include <stdint.h>

#include "odid.h"

namespace rx {

enum Transport : uint8_t { kWifiBeacon = 0, kWifiNan = 1, kBle = 2 };

// One decoded 25-byte message plus where it came from.
struct Frame {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t transport;
  uint8_t channel;  // WiFi channel, 0 for BLE
  uint8_t msg[odid::kMsgSize];
};

bool begin();

// Pops one received message. Non-blocking.
bool receive(Frame& f);

// Advances WiFi channel hopping; call from the main loop.
void tick(uint32_t now_ms);
void setHopping(bool on);
bool hopping();
uint8_t channel();

uint32_t droppedFrames();
const char* transportName(uint8_t t);

}  // namespace rx
