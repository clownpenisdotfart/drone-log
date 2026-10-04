// Decoder for ASTM F3411 (Open Drone ID) 25-byte messages.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace odid {

constexpr size_t kMsgSize = 25;

enum MsgType : uint8_t {
  kBasicId = 0x0,
  kLocation = 0x1,
  kAuth = 0x2,
  kSelfId = 0x3,
  kSystem = 0x4,
  kOperatorId = 0x5,
  kMessagePack = 0xF,
};

// Fields accumulated for one aircraft. Each message type fills its own
// subset; the *_valid flags say which subsets have been seen.
struct State {
  bool basic_valid = false;
  uint8_t id_type = 0;  // 1 serial, 2 CAA reg, 3 UTM UUID, 4 specific session
  uint8_t ua_type = 0;
  char uas_id[21] = {0};

  bool location_valid = false;
  uint8_t status = 0;      // 0 undeclared, 1 ground, 2 airborne, 3 emergency, 4 RID failure
  float direction = NAN;   // degrees true
  float speed_h = NAN;     // m/s
  float speed_v = NAN;     // m/s
  double lat = NAN;
  double lon = NAN;
  float alt_baro = NAN;    // m
  float alt_geo = NAN;     // m
  float height = NAN;      // m (above takeoff or ground, see height_type)
  uint8_t height_type = 0;

  bool self_id_valid = false;
  char self_id[24] = {0};

  bool system_valid = false;
  double op_lat = NAN;
  double op_lon = NAN;
  float op_alt = NAN;
  uint32_t system_ts = 0;  // seconds since 2019-01-01 00:00 UTC, 0 if absent

  bool operator_valid = false;
  char operator_id[21] = {0};
};

// Unix time of the ASTM F3411 epoch (2019-01-01T00:00:00Z).
constexpr uint32_t kEpoch2019 = 1546300800;

// Applies one 25-byte message to `s`. Returns the message type, or -1 if the
// message is not one we decode. Message packs must be unpacked by the caller.
int decode(const uint8_t* msg, State& s);

const char* uaTypeName(uint8_t t);
const char* statusName(uint8_t s);

}  // namespace odid
