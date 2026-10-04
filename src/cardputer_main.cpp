// Receive-only FAA Remote ID (ASTM F3411) logger for the M5Stack Cardputer ADV.
//
// Listens for Remote ID broadcasts over WiFi (beacon and NAN) and Bluetooth
// LE, shows heard aircraft on screen, and appends a timestamped CSV row per
// detection to the microSD card. The radio is never used to transmit.
//
// Keys: ; / .  (or up / down) select    enter  details    ` / esc  back
//       h  toggle channel hopping (off = lock to ch 6)       c  clear list
//       G0 button  same as enter

#include <M5Cardputer.h>

#include "drones.h"
#include "odid.h"
#include "rid_rx.h"
#include "sdlog.h"

namespace {

constexpr uint32_t kLogIntervalMs = 1000;    // max one CSV row per drone per second
constexpr uint32_t kStaleMs = 30 * 1000;     // grey out after this
constexpr uint32_t kExpireMs = 10 * 60 * 1000;
constexpr uint32_t kRedrawMs = 250;
constexpr uint32_t kFlushMs = 2000;
constexpr uint32_t kSdRetryMs = 5000;

constexpr int kW = 240;
constexpr int kH = 135;
constexpr int kHeaderH = 12;
constexpr int kFooterH = 10;
constexpr int kRowH = 21;
constexpr int kRows = (kH - kHeaderH - kFooterH) / kRowH;

M5Canvas g_canvas(&M5Cardputer.Display);
DroneTable g_table;
bool g_radio_ok = false;
int64_t g_utc_base_s = 0;  // Unix time at uptime 0, learned from System messages
uint8_t g_selected_mac[6] = {0};
int g_selected = 0;
bool g_detail = false;

uint32_t g_last_redraw = 0;
uint32_t g_last_flush = 0;
uint32_t g_last_sd_try = 0;

void macStr(const uint8_t* m, char* out, size_t n) {
  snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void ageStr(uint32_t ms, char* out, size_t n) {
  uint32_t s = ms / 1000;
  if (s < 60) {
    snprintf(out, n, "%lus", (unsigned long)s);
  } else if (s < 3600) {
    snprintf(out, n, "%lum", (unsigned long)(s / 60));
  } else {
    snprintf(out, n, "%luh", (unsigned long)(s / 3600));
  }
}

char transportTag(const Drone& d) {
  const bool wifi = d.transports & ((1 << rx::kWifiBeacon) | (1 << rx::kWifiNan));
  const bool ble = d.transports & (1 << rx::kBle);
  return wifi && ble ? '*' : (ble ? 'B' : 'W');
}

const char* displayId(const Drone& d, char* fallback, size_t n) {
  if (d.s.uas_id[0]) return d.s.uas_id;
  macStr(d.mac, fallback, n);
  return fallback;
}

uint16_t rowColor(const Drone& d, uint32_t now) {
  if (now - d.last_seen_ms > kStaleMs) return TFT_DARKGREY;
  if (d.s.location_valid && d.s.status == 3) return TFT_RED;
  if (d.s.location_valid && d.s.status == 2) return TFT_GREENYELLOW;
  return TFT_WHITE;
}

void drawHeader(uint32_t now) {
  g_canvas.fillRect(0, 0, kW, kHeaderH, g_radio_ok ? TFT_NAVY : TFT_MAROON);
  g_canvas.setTextColor(TFT_WHITE);
  g_canvas.setCursor(2, 2);
  char buf[64];
  snprintf(buf, sizeof(buf), "RID ch%02u%s n:%d", rx::channel(), rx::hopping() ? "~" : " ", g_table.count());
  g_canvas.print(buf);
  if (sdlog::ready()) {
    snprintf(buf, sizeof(buf), "%s %lu", sdlog::fileName(), (unsigned long)sdlog::rows());
  } else {
    snprintf(buf, sizeof(buf), "NO SD");
  }
  g_canvas.setTextColor(sdlog::ready() ? TFT_WHITE : TFT_ORANGE);
  g_canvas.setCursor(kW - 2 - g_canvas.textWidth(buf), 2);
  g_canvas.print(buf);
}

void drawFooter(const char* text) {
  g_canvas.fillRect(0, kH - kFooterH, kW, kFooterH, TFT_BLACK);
  g_canvas.setTextColor(TFT_DARKGREY);
  g_canvas.setCursor(2, kH - kFooterH + 1);
  g_canvas.print(text);
}

void drawList(Drone** list, int n, uint32_t now) {
  if (n == 0) {
    g_canvas.setTextColor(TFT_DARKGREY);
    g_canvas.setCursor(8, kHeaderH + 30);
    g_canvas.print(g_radio_ok ? "Listening for Remote ID..." : "Radio init failed");
    g_canvas.setCursor(8, kHeaderH + 44);
    g_canvas.print("WiFi beacon + NAN, BLE 4 legacy");
    return;
  }
  const int first = g_selected >= kRows ? g_selected - kRows + 1 : 0;
  char buf[64], id[24], age[8];
  for (int r = 0; r < kRows && first + r < n; r++) {
    const int i = first + r;
    const Drone& d = *list[i];
    const int y = kHeaderH + r * kRowH;
    if (i == g_selected) g_canvas.fillRect(0, y, kW, kRowH - 1, 0x18E3);
    g_canvas.setTextColor(rowColor(d, now));

    g_canvas.setCursor(2, y + 1);
    snprintf(buf, sizeof(buf), "%c %.20s", transportTag(d), displayId(d, id, sizeof(id)));
    g_canvas.print(buf);
    ageStr(now - d.last_seen_ms, age, sizeof(age));
    snprintf(buf, sizeof(buf), "%4d %4s", d.rssi, age);
    g_canvas.setCursor(kW - 2 - g_canvas.textWidth(buf), y + 1);
    g_canvas.print(buf);

    g_canvas.setCursor(10, y + 10);
    if (d.s.location_valid && !isnan(d.s.lat)) {
      snprintf(buf, sizeof(buf), "%.5f,%.5f %.0fm %.1fm/s", d.s.lat, d.s.lon,
               isnan(d.s.height) ? d.s.alt_geo : d.s.height, d.s.speed_h);
    } else {
      snprintf(buf, sizeof(buf), "%s  no position yet", d.s.basic_valid ? odid::uaTypeName(d.s.ua_type) : "");
    }
    g_canvas.print(buf);
  }
}

void line(int& y, const char* fmt, ...) {
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_canvas.setCursor(2, y);
  g_canvas.print(buf);
  y += 9;
}

void drawDetail(const Drone& d, uint32_t now) {
  const odid::State& s = d.s;
  char mac[20], age[8];
  macStr(d.mac, mac, sizeof(mac));
  ageStr(now - d.last_seen_ms, age, sizeof(age));
  int y = kHeaderH + 2;
  g_canvas.setTextColor(rowColor(d, now));
  line(y, "ID %s", s.uas_id[0] ? s.uas_id : "(none)");
  g_canvas.setTextColor(TFT_WHITE);
  line(y, "%s  idtype %u  %s", s.basic_valid ? odid::uaTypeName(s.ua_type) : "?", s.id_type,
       s.location_valid ? odid::statusName(s.status) : "");
  line(y, "%s %c %ddBm %s ago", mac, transportTag(d), d.rssi, age);
  line(y, "pos %.6f, %.6f", s.lat, s.lon);
  line(y, "alt geo %.1f baro %.1f hgt %.1f", s.alt_geo, s.alt_baro, s.height);
  line(y, "spd %.2f m/s  vs %.1f  dir %.0f", s.speed_h, s.speed_v, s.direction);
  line(y, "op  %.6f, %.6f", s.op_lat, s.op_lon);
  line(y, "opid %s", s.operator_id);
  line(y, "self %s", s.self_id);
  line(y, "msgs %lu", (unsigned long)d.msg_count);
}

void redraw(uint32_t now) {
  Drone* list[DroneTable::kMax];
  const int n = g_table.sorted(list, DroneTable::kMax);

  // Keep the selection on the same drone as the list re-sorts.
  for (int i = 0; i < n; i++) {
    if (memcmp(list[i]->mac, g_selected_mac, 6) == 0) {
      g_selected = i;
      break;
    }
  }
  if (g_selected >= n) g_selected = n > 0 ? n - 1 : 0;
  if (n > 0) memcpy(g_selected_mac, list[g_selected]->mac, 6);
  if (n == 0) g_detail = false;

  g_canvas.fillSprite(TFT_BLACK);
  g_canvas.setTextSize(1);
  drawHeader(now);
  if (g_detail) {
    drawDetail(*list[g_selected], now);
    drawFooter("esc back   ;/. prev/next");
  } else {
    drawList(list, n, now);
    drawFooter(";/. select  ent info  h hop  c clear");
  }
  g_canvas.pushSprite(0, 0);
}

void moveSelection(int delta) {
  Drone* list[DroneTable::kMax];
  const int n = g_table.sorted(list, DroneTable::kMax);
  if (n == 0) return;
  g_selected = (g_selected + delta + n) % n;
  memcpy(g_selected_mac, list[g_selected]->mac, 6);
}

void handleInput() {
  if (M5Cardputer.BtnA.wasPressed()) g_detail = !g_detail;
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  const auto& k = M5Cardputer.Keyboard.keysState();
  bool up = k.up, down = k.down, back = k.esc;
  for (char c : k.word) {
    switch (c) {
      case ';': up = true; break;
      case '.': down = true; break;
      case '`': back = true; break;
      case 'h':
      case 'H': rx::setHopping(!rx::hopping()); break;
      case 'c':
      case 'C':
        g_table.clear();
        g_selected = 0;
        g_detail = false;
        break;
    }
  }
  if (up) moveSelection(-1);
  if (down) moveSelection(1);
  if (k.enter) g_detail = !g_detail;
  if (back) g_detail = false;
}

void processFrames(uint32_t now) {
  rx::Frame f;
  for (int i = 0; i < 64 && rx::receive(f); i++) {
    Drone* d = g_table.apply(f, now);
    // System messages carry UTC; use the first one to timestamp the log.
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
    d->last_logged_ms = now;
    d->dirty = false;
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  g_canvas.setColorDepth(16);
  g_canvas.createSprite(kW, kH);

  sdlog::begin();
  g_radio_ok = rx::begin();
  redraw(millis());
}

void loop() {
  M5Cardputer.update();
  const uint32_t now = millis();

  handleInput();
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
  if (now - g_last_redraw >= kRedrawMs) {
    redraw(now);
    g_last_redraw = now;
  }
  delay(5);
}
