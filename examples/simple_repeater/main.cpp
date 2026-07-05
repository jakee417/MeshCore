#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#if defined(REQUIRE_WIFI_CREDS) && (!defined(WIFI_SSID) || !defined(WIFI_PWD))
  #error "Wi-Fi build requires WIFI_SSID and WIFI_PWD in platformio.local.ini"
#endif

#if defined(RP2040_PLATFORM)
  #include <helpers/rp2040/RP2040OTA.h>
#endif

#if defined(RP2040_PLATFORM) && defined(WIFI_SSID)
  #include <WiFi.h>

  uint8_t last_wifi_status = WL_IDLE_STATUS;
  unsigned long last_wifi_reconnect_attempt = 0;

  static void formatPublicKey(char* dest, size_t dest_size, const uint8_t* public_key) {
    if (dest_size < (PUB_KEY_SIZE * 2 + 1)) {
      if (dest_size > 0) {
        dest[0] = 0;
      }
      return;
    }
    mesh::Utils::toHex(dest, public_key, PUB_KEY_SIZE);
  }

  static void logWifiStatus(const char* prefix, uint8_t status) {
    Serial.printf("WiFi: %s status=%u", prefix, status);
    if (status == WL_CONNECTED) {
      Serial.printf(" ip=%s", WiFi.localIP().toString().c_str());
    } else {
      Serial.printf(" reason=%u", WiFi.reasonCode());
    }
    Serial.println();
  }

  static const char* getDefaultWifiHostnamePrefix() {
    #ifdef BOARD_NAME
    if (strcmp(BOARD_NAME, "rpipico2w") == 0) {
      return "Pico2W";
    }
    #endif

    return "PicoW";
  }

  static String setWifiHostname() {
    #ifdef WIFI_HOSTNAME
    WiFi.setHostname(WIFI_HOSTNAME);
    MESH_DEBUG_PRINTLN("WiFi hostname set to %s", WIFI_HOSTNAME);
    return String(WIFI_HOSTNAME);
    #else
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    const char* hostname_prefix = getDefaultWifiHostnamePrefix();

    const int suffix_len = 6;
    if (mac.length() < suffix_len) {
      MESH_DEBUG_PRINTLN("WiFi MAC address too short for hostname suffix: %s", mac.c_str());
      WiFi.setHostname(hostname_prefix);
      MESH_DEBUG_PRINTLN("WiFi hostname set to %s", hostname_prefix);
      return String(hostname_prefix);
    }

    const String hostname = String(hostname_prefix) + mac.substring(mac.length() - suffix_len);
    WiFi.setHostname(hostname.c_str());
    MESH_DEBUG_PRINTLN("WiFi hostname set to %s", hostname.c_str());
    return hostname;
    #endif
  }
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

#if defined(RP2040_PLATFORM) && defined(WIFI_SSID)
  const String wifi_hostname = setWifiHostname();
  char public_key[65];
  formatPublicKey(public_key, sizeof(public_key), the_mesh.self_id.pub_key);
  Serial.printf("WiFi: connecting to SSID %s as %s\n", WIFI_SSID, wifi_hostname.c_str());
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  logWifiStatus("begin", WiFi.status());
  mesh::rp2040ota::begin(board, the_mesh.getNodeName(), wifi_hostname.c_str(), public_key, "Repeater");
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#if defined(RP2040_PLATFORM) && defined(WIFI_SSID)
  uint8_t wifi_status = WiFi.status();
  if (wifi_status != last_wifi_status) {
    logWifiStatus("state-change", wifi_status);
    last_wifi_status = wifi_status;
  }

  if (wifi_status == WL_DISCONNECTED && (millis() - last_wifi_reconnect_attempt > 10000)) {
    Serial.println("WiFi: attempting reconnect");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PWD);
    logWifiStatus("reconnect", WiFi.status());
    last_wifi_reconnect_attempt = millis();
  }
#endif

  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
