// Table of aircraft seen recently, keyed by transmitter MAC and merged by
// UAS ID when one drone is heard over more than one transport.
#pragma once

#include <stdint.h>

#include "odid.h"
#include "rid_rx.h"

struct Drone {
  bool used = false;
  uint8_t mac[6] = {0};
  uint8_t transports = 0;  // bitmask of 1 << rx::Transport
  uint8_t last_transport = 0;
  int8_t rssi = 0;
  uint32_t first_seen_ms = 0;
  uint32_t last_seen_ms = 0;
  uint32_t last_logged_ms = 0;
  uint32_t msg_count = 0;
  bool dirty = false;  // changed since last CSV row
  odid::State s;
};

class DroneTable {
 public:
  static constexpr int kMax = 32;

  // Applies a received frame and returns the drone it belongs to.
  Drone* apply(const rx::Frame& f, uint32_t now_ms);

  // Drops entries not heard for `max_age_ms`.
  void expire(uint32_t now_ms, uint32_t max_age_ms);

  // Fills `out` with drones ordered most recently seen first; returns count.
  int sorted(Drone** out, int max) ;
  int count() const;
  Drone* at(int i) { return &drones_[i]; }
  void clear();

 private:
  Drone* findMac(const uint8_t* mac);
  Drone* allocate(uint32_t now_ms);
  void mergeById(Drone* d);

  Drone drones_[kMax];
};
