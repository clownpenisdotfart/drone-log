// Copy to include/node_config.h (git-ignored) and fill in to enable uploads.
// With NODE_WIFI_SSID left empty the node only logs to its SD card.
#pragma once

// Network the node joins briefly to upload what it heard.
#define NODE_WIFI_SSID ""
#define NODE_WIFI_PASSWORD ""

// Collector endpoint and this node's token (see server/README.md).
#define NODE_INGEST_URL "https://api.copcity.net/api/ingest"
#define NODE_TOKEN ""

// Seconds between uploads. WiFi capture pauses for a few seconds each time
// (Bluetooth keeps listening), so shorter is more live but hears a bit less.
#define NODE_UPLOAD_INTERVAL_S 60
