// Headless fixed-node build: same receive-only Remote ID capture as the
// Cardputer, logging to SD and (when configured) uploading aircraft tracks
// to the collector behind the copcity.net live map. No screen or keyboard.

#include <Arduino.h>

#include "drones.h"
#include "odid.h"
#include "rid_rx.h"
#include "sdlog.h"
#include "uplink.h"

namespace {

constexpr uint32_t kLogIntervalMs = 1000;  // max one row per drone per second
constexpr uint32_t kExpireMs = 10 * 60 * 1000;
constexpr uint32_t kFlushMs = 2000;
constexpr uint32_t kSdRetryMs = 5000;
constexpr uint32_t kStatusMs = 30000;

DroneTable g_table;
int64_t g_utc_base_s = 0;
uint32_t g_last_flush = 0;
uint32_t g_last_sd_try = 0;
uint32_t g_last_status = 0;

void processFrames(uint32_t now) {
  rx::Frame f;
  for (int i = 0; i < 128 && rx::receive(f); i++) {
    Drone* d = g_table.apply(f, now);
    if (g_utc_base_s == 0 && d->s.system_valid && d->s.system_ts != 0) {
      g_utc_base_s = (int64_t)odid::kEpoch2019 + d->s.system_ts - now / 1000;
    }
  }
}

void logDetections(uint32_t now) {
  for (int i = 0; i < DroneTable::kMax; i++) {
    Drone* d = g_table.at(i);
    if (!d->used || !d->dirty || d->msg_count == 0) continue;
    if (d->last_logged_ms != 0 && now - d->last_logged_ms < kLogIntervalMs) continue;
    sdlog::write(*d, now, g_utc_base_s);
    uplink::queue(*d, now);
    d->last_logged_ms = now;
    d->dirty = false;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("drone-log node: receive-only Remote ID logger");
  Serial.printf("SD: %s\n", sdlog::begin() ? sdlog::fileName() : "not found");
  Serial.printf("radio: %s\n", rx::begin() ? "ok" : "FAILED");
  Serial.printf("upload: %s\n", uplink::enabled() ? "on" : "off (SD only)");
}

void loop() {
  const uint32_t now = millis();
  rx::tick(now);
  processFrames(now);
  logDetections(now);
  g_table.expire(now, kExpireMs);

  if (now - g_last_flush >= kFlushMs) {
    sdlog::flush();
    g_last_flush = now;
  }
  if (!sdlog::ready() && now - g_last_sd_try >= kSdRetryMs) {
    sdlog::begin();
    g_last_sd_try = now;
  }
  uplink::tick(now);
  if (now - g_last_status >= kStatusMs) {
    Serial.printf("ch%u aircraft:%d rows:%lu sent:%lu pending:%u failed:%lu dropped:%lu\n", rx::channel(),
                  g_table.count(), (unsigned long)sdlog::rows(), (unsigned long)uplink::sent(),
                  (unsigned)uplink::pending(), (unsigned long)uplink::failed(), (unsigned long)rx::droppedFrames());
    g_last_status = now;
  }
  delay(5);
}
