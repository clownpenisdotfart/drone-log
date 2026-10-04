#include "uplink.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <string.h>

#include "rid_rx.h"

#if __has_include("node_config.h")
#include "node_config.h"
#else
#include "node_config.example.h"
#endif

namespace uplink {
namespace {

struct Det {
  uint32_t up_ms;
  char id[21];
  uint8_t transport;
  int8_t rssi;
  uint8_t ua_type;
  uint8_t status;
  double lat, lon;
  float alt_geo, height, speed, dir;
};

constexpr size_t kCap = 256;  // oldest entries are dropped if uploads fail
Det g_buf[kCap];
size_t g_head = 0;  // index of oldest
size_t g_count = 0;
uint32_t g_last_upload = 0;
uint32_t g_sent = 0;
uint32_t g_failed = 0;
constexpr uint32_t kHeartbeatMs = 5 * 60 * 1000;  // check in even when quiet

void appendNum(String& s, double v, int decimals) {
  if (isnan(v)) {
    s += "null";
  } else {
    s += String(v, decimals);
  }
}

void appendJsonString(String& s, const char* v) {
  s += '"';
  for (; *v; v++) {
    if (*v == '"' || *v == '\\') s += '\\';
    if ((uint8_t)*v >= 0x20) s += *v;
  }
  s += '"';
}

String buildBody(size_t n, uint32_t now_ms) {
  String s;
  s.reserve(96 + n * 170);
  s += "{\"uptime_ms\":";
  s += now_ms;
  s += ",\"tracks\":[";
  for (size_t i = 0; i < n; i++) {
    const Det& d = g_buf[(g_head + i) % kCap];
    if (i) s += ',';
    s += "{\"up_ms\":";
    s += d.up_ms;
    s += ",\"id\":";
    appendJsonString(s, d.id);
    s += ",\"transport\":\"";
    s += rx::transportName(d.transport);
    s += "\",\"rssi\":";
    s += d.rssi;
    s += ",\"ua_type\":\"";
    s += odid::uaTypeName(d.ua_type);
    s += "\",\"status\":\"";
    s += odid::statusName(d.status);
    s += "\",\"lat\":";
    appendNum(s, d.lat, 7);
    s += ",\"lon\":";
    appendNum(s, d.lon, 7);
    s += ",\"alt_geo\":";
    appendNum(s, d.alt_geo, 1);
    s += ",\"height\":";
    appendNum(s, d.height, 1);
    s += ",\"speed\":";
    appendNum(s, d.speed, 2);
    s += ",\"dir\":";
    appendNum(s, d.dir, 0);
    s += '}';
  }
  s += "]}";
  return s;
}

bool connect() {
  WiFi.begin(NODE_WIFI_SSID, NODE_WIFI_PASSWORD);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

bool post(const String& body) {
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, NODE_INGEST_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + NODE_TOKEN);
  const int code = http.POST(body);
  http.end();
  if (code < 200 || code >= 300) log_w("upload failed: HTTP %d", code);
  return code >= 200 && code < 300;
}

}  // namespace

bool enabled() { return NODE_WIFI_SSID[0] != '\0' && NODE_TOKEN[0] != '\0'; }

void queue(const Drone& src, uint32_t now_ms) {
  const odid::State& s = src.s;
  // Only aircraft with a position fix go on the map.
  if (!enabled() || !s.location_valid || isnan(s.lat) || isnan(s.lon)) return;
  if (g_count == kCap) {
    g_head = (g_head + 1) % kCap;
    g_count--;
  }
  Det& d = g_buf[(g_head + g_count) % kCap];
  g_count++;
  d.up_ms = now_ms;
  if (s.uas_id[0]) {
    memcpy(d.id, s.uas_id, sizeof(d.id));
  } else {
    snprintf(d.id, sizeof(d.id), "%02X%02X%02X%02X%02X%02X", src.mac[0], src.mac[1], src.mac[2], src.mac[3],
             src.mac[4], src.mac[5]);
  }
  d.transport = src.last_transport;
  d.rssi = src.rssi;
  d.ua_type = s.basic_valid ? s.ua_type : 0;
  d.status = s.status;
  d.lat = s.lat;
  d.lon = s.lon;
  d.alt_geo = s.alt_geo;
  d.height = s.height;
  d.speed = s.speed_h;
  d.dir = s.direction;
}

bool tick(uint32_t now_ms) {
  if (!enabled()) return false;
  const uint32_t since = now_ms - g_last_upload;
  const bool due = g_count > 0 ? since >= NODE_UPLOAD_INTERVAL_S * 1000UL : since >= kHeartbeatMs;
  if (!due) return false;
  g_last_upload = now_ms;

  const size_t n = g_count;
  const String body = buildBody(n, now_ms);

  rx::pauseWifi();
  const bool ok = connect() && post(body);
  WiFi.disconnect(false, false);
  rx::resumeWifi();

  if (ok) {
    // Drop what we sent; anything queued meanwhile stays.
    g_head = (g_head + n) % kCap;
    g_count -= n;
    g_sent += n;
  } else {
    g_failed++;
  }
  return true;
}

uint32_t sent() { return g_sent; }
uint32_t failed() { return g_failed; }
size_t pending() { return g_count; }

}  // namespace uplink
