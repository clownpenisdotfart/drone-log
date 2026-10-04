#include "drones.h"

#include <string.h>

Drone* DroneTable::findMac(const uint8_t* mac) {
  for (auto& d : drones_) {
    if (d.used && memcmp(d.mac, mac, 6) == 0) return &d;
  }
  return nullptr;
}

Drone* DroneTable::allocate(uint32_t now_ms) {
  Drone* oldest = nullptr;
  for (auto& d : drones_) {
    if (!d.used) return &d;
    if (!oldest || now_ms - d.last_seen_ms > now_ms - oldest->last_seen_ms) oldest = &d;
  }
  return oldest;  // table full: evict the stalest entry
}

// If another entry already carries this UAS ID (same drone heard on another
// transport with a different MAC), fold the older one into `d`.
void DroneTable::mergeById(Drone* d) {
  if (d->s.uas_id[0] == '\0') return;
  for (auto& o : drones_) {
    if (&o == d || !o.used || strcmp(o.s.uas_id, d->s.uas_id) != 0) continue;
    d->transports |= o.transports;
    d->msg_count += o.msg_count;
    if (o.first_seen_ms < d->first_seen_ms) d->first_seen_ms = o.first_seen_ms;
    if (!d->s.location_valid && o.s.location_valid) {
      d->s.location_valid = true;
      d->s.status = o.s.status;
      d->s.direction = o.s.direction;
      d->s.speed_h = o.s.speed_h;
      d->s.speed_v = o.s.speed_v;
      d->s.lat = o.s.lat;
      d->s.lon = o.s.lon;
      d->s.alt_baro = o.s.alt_baro;
      d->s.alt_geo = o.s.alt_geo;
      d->s.height = o.s.height;
      d->s.height_type = o.s.height_type;
    }
    if (!d->s.self_id_valid && o.s.self_id_valid) {
      memcpy(d->s.self_id, o.s.self_id, sizeof(d->s.self_id));
      d->s.self_id_valid = true;
    }
    if (!d->s.system_valid && o.s.system_valid) {
      d->s.op_lat = o.s.op_lat;
      d->s.op_lon = o.s.op_lon;
      d->s.op_alt = o.s.op_alt;
      d->s.system_ts = o.s.system_ts;
      d->s.system_valid = true;
    }
    if (!d->s.operator_valid && o.s.operator_valid) {
      memcpy(d->s.operator_id, o.s.operator_id, sizeof(d->s.operator_id));
      d->s.operator_valid = true;
    }
    o = Drone();
  }
}

Drone* DroneTable::apply(const rx::Frame& f, uint32_t now_ms) {
  Drone* d = findMac(f.mac);
  if (!d) {
    d = allocate(now_ms);
    *d = Drone();
    d->used = true;
    memcpy(d->mac, f.mac, 6);
    d->first_seen_ms = now_ms;
  }
  int type = odid::decode(f.msg, d->s);
  if (type < 0) return d;
  d->transports |= 1 << f.transport;
  d->last_transport = f.transport;
  d->rssi = f.rssi;
  d->last_seen_ms = now_ms;
  d->msg_count++;
  d->dirty = true;
  if (type == odid::kBasicId) mergeById(d);
  return d;
}

void DroneTable::expire(uint32_t now_ms, uint32_t max_age_ms) {
  for (auto& d : drones_) {
    if (d.used && now_ms - d.last_seen_ms > max_age_ms) d = Drone();
  }
}

int DroneTable::sorted(Drone** out, int max) {
  int n = 0;
  for (auto& d : drones_) {
    if (d.used && d.msg_count > 0 && n < max) out[n++] = &d;
  }
  // Insertion sort: tiny table.
  for (int i = 1; i < n; i++) {
    Drone* x = out[i];
    int j = i - 1;
    while (j >= 0 && out[j]->last_seen_ms < x->last_seen_ms) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = x;
  }
  return n;
}

int DroneTable::count() const {
  int n = 0;
  for (const auto& d : drones_) n += d.used && d.msg_count > 0;
  return n;
}

void DroneTable::clear() {
  for (auto& d : drones_) d = Drone();
}
