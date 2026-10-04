#include "rid_rx.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <host/ble_gap.h>
#include <string.h>

#include <atomic>

namespace rx {
namespace {

QueueHandle_t g_queue = nullptr;
std::atomic<uint32_t> g_dropped{0};

// Spend every other dwell on channel 6, where NAN Remote ID lives and most
// beacon Remote ID is seen, and sweep the rest of 2.4 GHz in between.
const uint8_t kHopSeq[] = {6, 1, 6, 2, 6, 3, 6, 4, 6, 5, 6, 7, 6, 8, 6, 9, 6, 10, 6, 11, 6, 12, 6, 13};
constexpr uint32_t kDwellMs = 150;
size_t g_hop_idx = 0;
uint32_t g_last_hop = 0;
bool g_hopping = true;
uint8_t g_channel = 6;
bool g_wifi_paused = false;

const wifi_promiscuous_filter_t kMgmtFilter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};

// ASTM F3411 vendor IE OUI + type in WiFi beacons.
const uint8_t kAstmOui[] = {0xFA, 0x0B, 0xBC, 0x0D};
// WiFi Alliance OUI + NAN type, and the NAN service ID for "org.asd-stan.rid".
const uint8_t kWfaNanOui[] = {0x50, 0x6F, 0x9A, 0x13};
const uint8_t kRidServiceId[] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};

void push(const uint8_t* mac, int8_t rssi, uint8_t transport, uint8_t channel, const uint8_t* msg) {
  Frame f;
  memcpy(f.mac, mac, 6);
  f.rssi = rssi;
  f.transport = transport;
  f.channel = channel;
  memcpy(f.msg, msg, odid::kMsgSize);
  if (xQueueSend(g_queue, &f, 0) != pdTRUE) g_dropped++;
}

// Handles either a single message or a message pack (type 0xF).
void pushMessages(const uint8_t* data, size_t len, const uint8_t* mac, int8_t rssi, uint8_t transport,
                  uint8_t channel) {
  if (len < odid::kMsgSize) return;
  if ((data[0] >> 4) != odid::kMessagePack) {
    push(mac, rssi, transport, channel, data);
    return;
  }
  if (len < 3 || data[1] != odid::kMsgSize) return;
  const uint8_t count = data[2];
  if (count > 9 || 3 + (size_t)count * odid::kMsgSize > len) return;
  for (uint8_t i = 0; i < count; i++) {
    push(mac, rssi, transport, channel, data + 3 + i * odid::kMsgSize);
  }
}

void parseBeacon(const uint8_t* p, size_t len, int8_t rssi, uint8_t channel) {
  // 24-byte MAC header + 12 bytes of fixed beacon fields, then tagged IEs.
  size_t off = 36;
  while (off + 2 <= len) {
    const uint8_t id = p[off];
    const uint8_t ie_len = p[off + 1];
    if (off + 2 + ie_len > len) break;
    const uint8_t* ie = p + off + 2;
    // Vendor IE: OUI(3) type(1) counter(1) message pack.
    if (id == 0xDD && ie_len > 5 && memcmp(ie, kAstmOui, sizeof(kAstmOui)) == 0) {
      pushMessages(ie + 5, ie_len - 5, p + 10, rssi, kWifiBeacon, channel);
    }
    off += 2 + ie_len;
  }
}

void parseNanAction(const uint8_t* p, size_t len, int8_t rssi, uint8_t channel) {
  // Public action (4), vendor specific (9), WFA OUI, NAN type 0x13.
  if (len < 30 || p[24] != 0x04 || p[25] != 0x09 || memcmp(p + 26, kWfaNanOui, 4) != 0) return;
  size_t off = 30;
  while (off + 3 <= len) {
    const uint8_t attr_id = p[off];
    const uint16_t attr_len = p[off + 1] | (p[off + 2] << 8);
    const uint8_t* a = p + off + 3;
    if (off + 3 + attr_len > len) break;
    // Service Descriptor Attribute: service ID(6) instance(1) requestor(1)
    // control(1) service-info length(1) then counter(1) + message pack.
    if (attr_id == 0x03 && attr_len >= 11 && memcmp(a, kRidServiceId, 6) == 0) {
      const uint8_t info_len = a[9];
      if (10 + (size_t)info_len <= attr_len && info_len > 1) {
        pushMessages(a + 11, info_len - 1, p + 10, rssi, kWifiNan, channel);
      }
    }
    off += 3 + attr_len;
  }
}

void onWifiPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const auto* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len - 4;  // drop FCS
  if (len < 24) return;
  const int8_t rssi = pkt->rx_ctrl.rssi;
  const uint8_t channel = pkt->rx_ctrl.channel;
  if (p[0] == 0x80) {
    parseBeacon(p, len, rssi, channel);
  } else if (p[0] == 0xD0) {
    parseNanAction(p, len, rssi, channel);
  }
}

int onBleEvent(struct ble_gap_event* event, void*) {
  if (event->type != BLE_GAP_EVENT_DISC) return 0;
  const auto& d = event->disc;
  const uint8_t* p = d.data;
  const size_t len = d.length_data;
  // NimBLE stores addresses little-endian; flip to display order.
  uint8_t mac[6];
  for (int i = 0; i < 6; i++) mac[i] = d.addr.val[5 - i];

  size_t off = 0;
  while (off + 1 < len) {
    const uint8_t ad_len = p[off];
    if (ad_len == 0 || off + 1 + ad_len > len) break;
    const uint8_t ad_type = p[off + 1];
    const uint8_t* ad = p + off + 2;
    // Service data, UUID 0xFFFA (ASTM), app code 0x0D, counter, message.
    if (ad_type == 0x16 && ad_len >= 5 && ad[0] == 0xFA && ad[1] == 0xFF && ad[2] == 0x0D) {
      pushMessages(ad + 4, ad_len - 5, mac, d.rssi, kBle, 0);
    }
    off += 1 + ad_len;
  }
  return 0;
}

bool startBle() {
  BLEDevice::init("");  // host only; we never advertise or connect
  ble_gap_disc_params params = {};
  params.passive = 1;            // no scan requests: receive only
  params.filter_duplicates = 0;  // Remote ID updates arrive as repeats
  params.itvl = 0x60;            // 60 ms
  params.window = 0x60;          // 100% duty (shared with WiFi via coex)
  uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
  int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, onBleEvent, nullptr);
  if (rc != 0) log_e("ble_gap_disc failed: %d", rc);
  return rc == 0;
}

bool startWifi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_STA)) return false;
  WiFi.disconnect(false, false);
  esp_wifi_set_promiscuous_filter(&kMgmtFilter);
  esp_wifi_set_promiscuous_rx_cb(&onWifiPacket);
  if (esp_wifi_set_promiscuous(true) != ESP_OK) return false;
  esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
  return true;
}

}  // namespace

bool begin() {
  g_queue = xQueueCreate(128, sizeof(Frame));
  bool wifi_ok = startWifi();
  bool ble_ok = startBle();
  return wifi_ok && ble_ok;
}

bool receive(Frame& f) { return g_queue && xQueueReceive(g_queue, &f, 0) == pdTRUE; }

void tick(uint32_t now_ms) {
  if (!g_hopping || g_wifi_paused || now_ms - g_last_hop < kDwellMs) return;
  g_last_hop = now_ms;
  g_hop_idx = (g_hop_idx + 1) % sizeof(kHopSeq);
  g_channel = kHopSeq[g_hop_idx];
  esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
}

void setHopping(bool on) {
  g_hopping = on;
  if (!on) {
    g_channel = 6;
    esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
  }
}

bool hopping() { return g_hopping; }
uint8_t channel() { return g_channel; }
uint32_t droppedFrames() { return g_dropped; }

void pauseWifi() {
  g_wifi_paused = true;
  esp_wifi_set_promiscuous(false);
}

void resumeWifi() {
  g_wifi_paused = false;
  esp_wifi_set_promiscuous_filter(&kMgmtFilter);
  esp_wifi_set_promiscuous_rx_cb(&onWifiPacket);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
}

const char* transportName(uint8_t t) {
  switch (t) {
    case kWifiBeacon: return "wifi-beacon";
    case kWifiNan: return "wifi-nan";
    case kBle: return "ble";
    default: return "?";
  }
}

}  // namespace rx
