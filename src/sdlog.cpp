#include "sdlog.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>

namespace sdlog {
namespace {

// microSD wiring; defaults match the Cardputer / Cardputer ADV and other
// boards override them with build flags.
#ifndef SD_SCK_PIN
#define SD_SCK_PIN 40
#define SD_MISO_PIN 39
#define SD_MOSI_PIN 14
#define SD_CS_PIN 12
#endif
constexpr int kSck = SD_SCK_PIN;
constexpr int kMiso = SD_MISO_PIN;
constexpr int kMosi = SD_MOSI_PIN;
constexpr int kCs = SD_CS_PIN;

SPIClass g_spi(HSPI);
File g_file;
bool g_ready = false;
bool g_spi_started = false;
char g_name[32] = "";
uint32_t g_rows = 0;

const char kHeader[] =
    "uptime_ms,utc,transport,mac,rssi_dbm,uas_id,id_type,ua_type,status,lat,lon,"
    "alt_geo_m,alt_baro_m,height_m,speed_h_mps,speed_v_mps,direction_deg,"
    "op_lat,op_lon,op_alt_m,operator_id,self_id,msg_count\n";

// Writes a double-quoted CSV field, doubling embedded quotes.
void quoted(const char* s) {
  g_file.print('"');
  for (; *s; s++) {
    if (*s == '"') g_file.print('"');
    g_file.print(*s);
  }
  g_file.print('"');
}

void num(double v, int decimals) {
  if (!isnan(v)) g_file.print(v, decimals);
}

}  // namespace

bool begin() {
  if (g_ready) return true;
  if (!g_spi_started) {
    g_spi.begin(kSck, kMiso, kMosi, kCs);
    g_spi_started = true;
  }
  SD.end();
  if (!SD.begin(kCs, g_spi, 25000000)) return false;
  if (!SD.exists("/remoteid")) SD.mkdir("/remoteid");
  for (int i = 1; i <= 9999; i++) {
    snprintf(g_name, sizeof(g_name), "/remoteid/rid_%04d.csv", i);
    if (!SD.exists(g_name)) break;
  }
  g_file = SD.open(g_name, FILE_WRITE);
  if (!g_file) return false;
  g_file.print(kHeader);
  g_file.flush();
  g_rows = 0;
  g_ready = true;
  return true;
}

bool ready() { return g_ready; }
const char* fileName() { return g_ready ? g_name + 10 : "none"; }  // skip "/remoteid/"
uint32_t rows() { return g_rows; }

void write(const Drone& d, uint32_t now_ms, int64_t utc_base_s) {
  if (!g_ready) return;
  const odid::State& s = d.s;
  char buf[32];

  g_file.print(now_ms);
  g_file.print(',');
  if (utc_base_s > 0) {
    time_t t = (time_t)(utc_base_s + now_ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    g_file.print(buf);
  }
  g_file.print(',');
  g_file.print(rx::transportName(d.last_transport));
  g_file.print(',');
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4],
           d.mac[5]);
  g_file.print(buf);
  g_file.print(',');
  g_file.print(d.rssi);
  g_file.print(',');
  quoted(s.uas_id);
  g_file.print(',');
  if (s.basic_valid) g_file.print(s.id_type);
  g_file.print(',');
  if (s.basic_valid) g_file.print(odid::uaTypeName(s.ua_type));
  g_file.print(',');
  if (s.location_valid) g_file.print(odid::statusName(s.status));
  g_file.print(',');
  num(s.lat, 7);
  g_file.print(',');
  num(s.lon, 7);
  g_file.print(',');
  num(s.alt_geo, 1);
  g_file.print(',');
  num(s.alt_baro, 1);
  g_file.print(',');
  num(s.height, 1);
  g_file.print(',');
  num(s.speed_h, 2);
  g_file.print(',');
  num(s.speed_v, 1);
  g_file.print(',');
  num(s.direction, 0);
  g_file.print(',');
  num(s.op_lat, 7);
  g_file.print(',');
  num(s.op_lon, 7);
  g_file.print(',');
  num(s.op_alt, 1);
  g_file.print(',');
  quoted(s.operator_id);
  g_file.print(',');
  quoted(s.self_id);
  g_file.print(',');
  g_file.print(d.msg_count);
  if (g_file.print('\n') != 1) {
    // Card pulled or full: stop and let the main loop retry begin().
    g_file.close();
    g_ready = false;
    return;
  }
  g_rows++;
}

void flush() {
  if (g_ready) g_file.flush();
}

}  // namespace sdlog
