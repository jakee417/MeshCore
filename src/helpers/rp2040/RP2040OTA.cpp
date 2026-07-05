#include "RP2040OTA.h"

#if defined(RP2040_PLATFORM) && defined(PICO_CYW43_SUPPORTED)

#include <LittleFS.h>
#include <HTTPUpdateServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <stdarg.h>

namespace mesh::rp2040ota {

static WebServer* s_server = nullptr;
static HTTPUpdateServer* s_update_server = nullptr;
static MainBoard* s_board = nullptr;
static char s_node_id[64] = {0};
static char s_board_name[48] = {0};
static char s_hostname[64] = {0};
static char s_public_key[65] = {0};
static char s_contact_type[24] = {0};
static bool s_server_started = false;

static void otaLog(const char* fmt, ...) {
  char msg[196];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.printf("RP2040 OTA: %s\n", msg);
}

static bool getFsInfo(FSInfo& info) {
  if (!LittleFS.begin()) {
    otaLog("ERROR failed to mount LittleFS");
    return false;
  }

  LittleFS.info(info);
  return true;
}

static String fsCountToString(uint64_t value) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return String(buffer);
}

static String ipToString(const IPAddress& ip) {
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return String(buffer);
}

static void appendEscapedHtml(String& dest, const char* src, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    switch (src[i]) {
      case '&': dest += "&amp;"; break;
      case '<': dest += "&lt;"; break;
      case '>': dest += "&gt;"; break;
      default: dest += src[i]; break;
    }
  }
}

static void refreshDeviceMetadata(MainBoard& board, const char* id, const char* hostname, const char* public_key, const char* contact_type) {
  s_board = &board;
  snprintf(s_node_id, sizeof(s_node_id), "%s", id ? id : "Unknown");
  snprintf(s_board_name, sizeof(s_board_name), "%s", board.getManufacturerName());
  snprintf(s_hostname, sizeof(s_hostname), "%s", hostname && hostname[0] ? hostname : "Unknown");
  snprintf(s_public_key, sizeof(s_public_key), "%s", public_key && public_key[0] ? public_key : "Unknown");
  snprintf(s_contact_type, sizeof(s_contact_type), "%s", contact_type && contact_type[0] ? contact_type : "Unknown");
}

static bool ensureServerObjects() {
  if (!s_server) {
    s_server = new WebServer(80);
  }
  if (!s_update_server) {
    s_update_server = new HTTPUpdateServer();
  }
  return s_server && s_update_server;
}

static void handleHomePage() {
  otaLog("GET / from %s", s_server->client().remoteIP().toString().c_str());

  FSInfo fs_info;
  const bool have_fs_info = getFsInfo(fs_info);

  String temp_text = "n/a";
  if (s_board) {
    const float t = s_board->getMCUTemperature();
    if (!isnan(t)) {
      temp_text = String(t, 2);
      temp_text += " C (";
      temp_text += String((t * 9.0f / 5.0f) + 32.0f, 2);
      temp_text += " F)";
    }
  }

  String voltage_text = "n/a";
  if (s_board) {
    const uint16_t mv = s_board->getBattMilliVolts();
    if (mv > 0) {
      voltage_text = String(mv / 1000.0f, 3);
      voltage_text += " V";
    }
  }

  String home_page_html;
  home_page_html.reserve(1800);
  home_page_html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  home_page_html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  home_page_html += "<title>MeshCore Home</title></head><body>";
  home_page_html += "<h2>MeshCore Home</h2>";
  home_page_html += "<p><b>Node:</b> ";
  home_page_html += s_node_id;
  home_page_html += "<br><b>Public Key:</b> ";
  home_page_html += s_public_key;
  home_page_html += "<br><b>Type:</b> ";
  home_page_html += s_contact_type;
  home_page_html += "<br><b>Hostname:</b> ";
  home_page_html += s_hostname;
  home_page_html += "<br><b>Board:</b> ";
  home_page_html += s_board_name;
  home_page_html += "<br><b>Station IP:</b> ";
  home_page_html += WiFi.status() == WL_CONNECTED ? ipToString(WiFi.localIP()) : String("Not connected");
  home_page_html += "<br><b>MCU Temp:</b> ";
  home_page_html += temp_text;
  home_page_html += "<br><b>Voltage:</b> ";
  home_page_html += voltage_text;
  if (have_fs_info) {
    home_page_html += "<br><b>LittleFS Used:</b> ";
    home_page_html += fsCountToString(fs_info.usedBytes);
    home_page_html += " / ";
    home_page_html += fsCountToString(fs_info.totalBytes);
    home_page_html += " bytes";
    home_page_html += "<br><b>LittleFS Free:</b> ";
    home_page_html += fsCountToString(fs_info.totalBytes - fs_info.usedBytes);
    home_page_html += " bytes";
  }
  home_page_html += "</p>";
  home_page_html += "<p><a href='/log'>Logs</a></p>";
  home_page_html += "<p><a href='/status'>JSON Status</a></p>";
  home_page_html += "<p><a href='/update'>OTA Updater</a></p>";
  if (LittleFS.exists("/packet_log")) {
    home_page_html += "<p><a href='/clear-log'>Clear Packet Log</a></p>";
  }
  home_page_html += "</body></html>";

  s_server->send(200, "text/html", home_page_html);
}

static void handleStatus() {
  otaLog("GET /status from %s", s_server->client().remoteIP().toString().c_str());

  FSInfo fs_info;
  const bool have_fs_info = getFsInfo(fs_info);

  float t = NAN;
  uint16_t mv = 0;
  if (s_board) {
    t = s_board->getMCUTemperature();
    mv = s_board->getBattMilliVolts();
  }

  String json = "{";
  json += "\"node\":\"";
  json += s_node_id;
  json += "\",\"hostname\":\"";
  json += s_hostname;
  json += "\",\"public_key\":\"";
  json += s_public_key;
  json += "\",\"type\":\"";
  json += s_contact_type;
  json += "\",\"board\":\"";
  json += s_board_name;
  json += "\",\"wifi_connected\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += ",\"station_ip\":";
  if (WiFi.status() == WL_CONNECTED) {
    json += "\"";
    json += ipToString(WiFi.localIP());
    json += "\"";
  } else {
    json += "null";
  }
  json += ",\"ota_upload_enabled\":true";
  json += ",\"battery_mv\":";
  json += String(mv);
  json += ",\"mcu_temp_c\":";
  if (isnan(t)) {
    json += "null";
  } else {
    json += String(t, 2);
  }
  json += ",\"littlefs_total_bytes\":";
  json += have_fs_info ? fsCountToString(fs_info.totalBytes) : String("null");
  json += ",\"littlefs_used_bytes\":";
  json += have_fs_info ? fsCountToString(fs_info.usedBytes) : String("null");
  json += ",\"littlefs_free_bytes\":";
  json += have_fs_info ? fsCountToString(fs_info.totalBytes - fs_info.usedBytes) : String("null");
  json += "}";

  s_server->send(200, "application/json", json);
}

static void handleClearPacketLog() {
  otaLog("GET /clear-log from %s", s_server->client().remoteIP().toString().c_str());

  String html;
  html.reserve(320);
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>MeshCore Logs</title></head><body>";
  html += "<h2>MeshCore Logs</h2>";

  if (!LittleFS.exists("/packet_log")) {
    html += "<p>No packet log to clear.</p>";
    html += "<p><a href='/'>Home</a> | <a href='/log'>Logs</a></p>";
    html += "</body></html>";
    s_server->send(200, "text/html", html);
    return;
  }

  if (!LittleFS.remove("/packet_log")) {
    otaLog("failed to remove /packet_log");
    html += "<p>Failed to clear packet log.</p>";
    html += "<p><a href='/'>Home</a> | <a href='/log'>Logs</a></p>";
    html += "</body></html>";
    s_server->send(500, "text/html", html);
    return;
  }

  otaLog("cleared /packet_log");
  html += "<p>Cleared packet log.</p>";
  html += "<p><a href='/'>Home</a> | <a href='/log'>Logs</a></p>";
  html += "</body></html>";
  s_server->send(200, "text/html", html);
}

static void handlePacketLog() {
  otaLog("GET /log from %s", s_server->client().remoteIP().toString().c_str());

  String html;
  html.reserve(4096);
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>MeshCore Logs</title></head><body>";
  html += "<h2>MeshCore Logs</h2>";
  html += "<p><a href='/'>Home</a> | <a href='/clear-log'>Clear Log</a></p>";

  if (!LittleFS.exists("/packet_log")) {
    otaLog("/log requested, but /packet_log does not exist");
    html += "<p>No packet log.</p></body></html>";
    s_server->send(404, "text/html", html);
    return;
  }

  File f = LittleFS.open("/packet_log", "r");
  if (!f) {
    otaLog("/log requested, failed to open /packet_log");
    html += "<p>Failed to open packet log.</p></body></html>";
    s_server->send(500, "text/html", html);
    return;
  }

  html += "<pre>";
  char buffer[256];
  while (f.available()) {
    const size_t len = f.readBytes(buffer, sizeof(buffer));
    appendEscapedHtml(html, buffer, len);
  }
  html += "</pre></body></html>";

  s_server->send(200, "text/html", html);
  f.close();
}

bool begin(MainBoard& board, const char* id, const char* hostname, const char* public_key, const char* contact_type) {
  refreshDeviceMetadata(board, id, hostname, public_key, contact_type);

  if (!ensureServerObjects()) {
    otaLog("ERROR failed to create server objects");
    return false;
  }

  if (s_server_started) {
    return true;
  }

  if (LittleFS.begin() && LittleFS.exists("firmware.bin")) {
    otaLog("removing stale firmware.bin before starting OTA server");
    LittleFS.remove("firmware.bin");
  }

  s_server->on("/", HTTP_GET, handleHomePage);
  s_server->on("/status", HTTP_GET, handleStatus);
  s_server->on("/log", HTTP_GET, handlePacketLog);
  s_server->on("/clear-log", HTTP_GET, handleClearPacketLog);
  s_update_server->setup(s_server, "/update");
  s_server->begin();

  s_server_started = true;
  otaLog("HTTP server started on port 80, routes: / /status /log /update");
  return true;
}

bool start(MainBoard& board, const char* id, char reply[]) {
  otaLog("start requested, node=%s board=%s", id ? id : "Unknown", board.getManufacturerName());
  refreshDeviceMetadata(board, id, s_hostname[0] ? s_hostname : nullptr, s_public_key[0] ? s_public_key : nullptr, s_contact_type[0] ? s_contact_type : nullptr);

  if (!begin(board, id, s_hostname, s_public_key, s_contact_type)) {
    strcpy(reply, "Error: failed to start HTTP server");
    return false;
  }

  const IPAddress ip = WiFi.localIP();
  sprintf(reply, "Ready: http://%u.%u.%u.%u/update", ip[0], ip[1], ip[2], ip[3]);
  otaLog("reply=%s", reply);
  MESH_DEBUG_PRINTLN("startOTAUpdate: %s", reply);
  return true;
}

void loop() {
  if (s_server_started && s_server) {
    s_server->handleClient();
  }
}

bool isRunning() {
  return s_server_started;
}

} // namespace mesh::rp2040ota

#else

namespace mesh::rp2040ota {

bool begin(MainBoard& board, const char* id, const char* hostname, const char* public_key, const char* contact_type) {
  (void)board;
  (void)id;
  (void)hostname;
  (void)public_key;
  (void)contact_type;
  return false;
}

bool start(MainBoard& board, const char* id, char reply[]) {
  (void)board;
  (void)id;
  (void)reply;
  return false;
}

void loop() {
}

bool isRunning() {
  return false;
}

} // namespace mesh::rp2040ota

#endif
