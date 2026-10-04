# drone-log

Receive-only FAA Remote ID (ASTM F3411 / Open Drone ID) logger for the
M5Stack Cardputer ADV (ESP32-S3).

It listens for Remote ID broadcasts, shows the aircraft it hears on screen,
and appends a timestamped CSV row per detection to the microSD card. The
radios are only ever used to receive: WiFi runs in promiscuous mode without
connecting or scanning, and BLE scanning is passive (no scan requests).

## What it hears

| Transport | Status |
|---|---|
| WiFi beacon (vendor IE `FA:0B:BC`, type `0x0D`) | supported, hops 2.4 GHz ch 1-13 with extra dwell on ch 6 |
| WiFi NAN (service `org.asd-stan.rid`) | supported (ch 6) |
| Bluetooth 4 legacy advertising (service data `0xFFFA`) | supported |
| Bluetooth 5 long range / extended advertising | not yet: the stock Arduino core ships NimBLE without extended scanning |

Decoded messages: Basic ID, Location/Vector, Self ID, System, Operator ID,
and Message Packs containing them. Authentication messages are ignored.

## Screen and keys

The list shows one aircraft per two lines, most recently heard first:
transport (`W` WiFi, `B` BLE, `*` both), UAS ID (or MAC until a Basic ID
arrives), RSSI, time since last heard, then position, height and speed.
Green is airborne, red is emergency, grey has not been heard for 30 s.

| Key | Action |
|---|---|
| `;` / `.` (up / down) | move selection |
| `enter` or the G0 button | details for the selected aircraft |
| `` ` `` (esc) | back to the list |
| `h` | toggle channel hopping (off locks WiFi to ch 6) |
| `c` | clear the list |

## CSV log

Each boot writes a new `/remoteid/rid_NNNN.csv`. A row is written whenever an
aircraft's data changes, at most once per second per aircraft. Columns:

```
uptime_ms,utc,transport,mac,rssi_dbm,uas_id,id_type,ua_type,status,lat,lon,
alt_geo_m,alt_baro_m,height_m,speed_h_mps,speed_v_mps,direction_deg,
op_lat,op_lon,op_alt_m,operator_id,self_id,msg_count
```

The Cardputer has no network time, so `utc` is filled in once the first
Remote ID System message (which carries a UTC timestamp) has been heard;
before that only `uptime_ms` is set. Empty cells mean the field was not
broadcast. If the card is missing or pulled, the header shows `NO SD` and
the logger retries every 5 s.

## Build and flash

```
pio run                 # build
pio run -t upload       # flash over USB-C
pio device monitor      # serial log
```

The project uses the pioarduino build of platform-espressif32 (Arduino
core 3.x) and fetches every library from GitHub.
