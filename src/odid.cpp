#include "odid.h"

#include <string.h>

namespace odid {
namespace {

int32_t le32s(const uint8_t* p) {
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

uint32_t le32(const uint8_t* p) { return (uint32_t)le32s(p); }

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Altitudes are encoded as (meters + 1000) * 2; 0 means unknown.
float altitude(const uint8_t* p) {
  uint16_t raw = le16(p);
  return raw == 0 ? NAN : raw * 0.5f - 1000.0f;
}

double coord(const uint8_t* p) {
  int32_t raw = le32s(p);
  return raw == 0 ? NAN : raw * 1e-7;
}

// Copies a fixed-width, NUL-padded ASCII field and strips non-printables.
void copyText(char* dst, const uint8_t* src, size_t n) {
  size_t len = 0;
  for (size_t i = 0; i < n && src[i]; i++) {
    char c = (char)src[i];
    dst[len++] = (c >= 0x20 && c < 0x7f) ? c : '?';
  }
  while (len > 0 && dst[len - 1] == ' ') len--;
  dst[len] = '\0';
}

}  // namespace

int decode(const uint8_t* m, State& s) {
  const uint8_t type = m[0] >> 4;
  switch (type) {
    case kBasicId: {
      const uint8_t id_type = m[1] >> 4;
      char id[21];
      copyText(id, m + 2, 20);
      // A drone may broadcast several Basic IDs (e.g. serial and session ID);
      // prefer the serial number once we have seen it.
      if (id[0] != '\0' && (s.uas_id[0] == '\0' || id_type == 1 || s.id_type != 1)) {
        memcpy(s.uas_id, id, sizeof(id));
        s.id_type = id_type;
      }
      s.ua_type = m[1] & 0x0f;
      s.basic_valid = true;
      return type;
    }
    case kLocation: {
      const uint8_t flags = m[1];
      s.status = flags >> 4;
      s.height_type = (flags >> 2) & 1;
      const bool ew = flags & 0x02;
      const bool mult = flags & 0x01;

      unsigned dir = m[2] + (ew ? 180u : 0u);
      s.direction = dir <= 360 ? (float)dir : NAN;

      if (m[3] == 255) {
        s.speed_h = NAN;
      } else {
        s.speed_h = mult ? m[3] * 0.75f + 255 * 0.25f : m[3] * 0.25f;
      }
      int8_t vs = (int8_t)m[4];
      s.speed_v = vs == 126 ? NAN : vs * 0.5f;

      s.lat = coord(m + 5);
      s.lon = coord(m + 9);
      s.alt_baro = altitude(m + 13);
      s.alt_geo = altitude(m + 15);
      s.height = altitude(m + 17);
      s.location_valid = true;
      return type;
    }
    case kSelfId:
      copyText(s.self_id, m + 2, 23);
      s.self_id_valid = true;
      return type;
    case kSystem:
      s.op_lat = coord(m + 2);
      s.op_lon = coord(m + 6);
      s.op_alt = altitude(m + 18);
      s.system_ts = le32(m + 20);
      s.system_valid = true;
      return type;
    case kOperatorId:
      copyText(s.operator_id, m + 2, 20);
      s.operator_valid = true;
      return type;
    default:
      return -1;
  }
}

const char* uaTypeName(uint8_t t) {
  static const char* const names[] = {
      "none",      "aeroplane", "multirotor", "gyroplane", "vtol",   "ornithopter",
      "glider",    "kite",      "balloon",    "airship",   "chute",  "rocket",
      "tethered",  "ground",    "other",
  };
  return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

const char* statusName(uint8_t s) {
  static const char* const names[] = {"undecl", "ground", "airborne", "EMERG", "RIDfail"};
  return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

}  // namespace odid
