// ============================================================================
// TankMeshMainDisplay v3.0 — CYD ESP32 3.5" (ST7796 + XPT2046 touch)
//
// Passively scans BLE advertising for up to 9 TankMesh sensors (no connection
// needed just to see levels), and briefly connects to a sensor only when it's
// new/unnamed or its settingsVersion has changed, to pull its name/color, or
// when you've assigned a name/color on the WiFi config portal and it needs
// writing back.
//
// Screens: Overview (all tanks, text list) -> one screen per tank (graphical
// level + %) -> Setup. Swipe/tap left-right to cycle, same gesture as v2.
//
// Libraries: LovyanGFX, BLE (BLEDevice/BLEScan/BLEClient/BLEAdvertisedDevice),
// WiFi, WebServer, Preferences — all the same libraries v2 already used.
// ============================================================================

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include "esp_coexist.h"
#include "TankMeshProtocol.h"

// ---------------- Display driver (unchanged from v2) ----------------
class LGFX_ESP32_35E : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel_instance; lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance; lgfx::Touch_XPT2046 _touch_instance;
public:
  LGFX_ESP32_35E() {
    auto bus = _bus_instance.config();
    bus.spi_host = VSPI_HOST; bus.freq_write = 40000000; bus.freq_read = 16000000;
    bus.pin_sclk = 14; bus.pin_mosi = 13; bus.pin_miso = 12; bus.pin_dc = 2;
    _bus_instance.config(bus); _panel_instance.setBus(&_bus_instance);
    auto p = _panel_instance.config();
    p.pin_cs = 15; p.pin_rst = -1; p.panel_width = 320; p.panel_height = 480;
    p.invert = false; p.rgb_order = false; p.bus_shared = true; _panel_instance.config(p);
    auto l = _light_instance.config(); l.pin_bl = 27; l.freq = 10000; l.pwm_channel = 1;
    _light_instance.config(l); _panel_instance.setLight(&_light_instance);
    auto t = _touch_instance.config();
    t.spi_host = VSPI_HOST; t.pin_cs = 33; t.pin_sclk = 14; t.pin_mosi = 13; t.pin_miso = 12; t.pin_int = 36;
    t.x_min = 300; t.x_max = 3900; t.y_min = 200; t.y_max = 3800;
    _touch_instance.config(t); _panel_instance.setTouch(&_touch_instance); setPanel(&_panel_instance);
  }
};
LGFX_ESP32_35E lcd;
#define SCREEN_W 320
#define SCREEN_H 480

// ---------------- Tank data model ----------------
#define MAX_TANKS 9

struct TankSlot {
  bool     inUse = false;
  uint8_t  mac[6] = {0};
  char     name[16] = "";
  uint32_t color = 0x808080;
  uint8_t  levelPercent = 0;
  uint16_t batteryMv = 0;
  uint32_t counter = 0;
  uint8_t  advSettingsVersion = 0;
  uint8_t  cachedSettingsVersion = 0xFE; // sentinel forces first fetch
  unsigned long lastSeenMs = 0;
  unsigned long lastAttemptMs = 0;
  bool     pendingFetch = false;
  bool     pendingWrite = false;      // name/color
  bool     pendingWriteCal = false;   // calibration
  bool     pendingWriteMesh = false;  // group label + wake interval
  TankMeshNameColor writeNameColor;
  TankMeshCal        writeCal;
  TankMeshMesh        writeMesh;
  TankMeshCal        cachedCal;
  TankMeshMesh        cachedMesh;
  bool     calMeshKnown = false; // true once cachedCal/cachedMesh reflect the real sensor
};
TankSlot tanks[MAX_TANKS];
int tankCount = 0;

int findSlotByMac(const uint8_t *mac) {
  for (int i = 0; i < tankCount; i++)
    if (memcmp(tanks[i].mac, mac, 6) == 0) return i;
  return -1;
}

String macToStr(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(buf);
}

bool strToMac(const String &s, uint8_t *mac) {
  int vals[6];
  if (sscanf(s.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
             &vals[0],&vals[1],&vals[2],&vals[3],&vals[4],&vals[5]) != 6) return false;
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)vals[i];
  return true;
}

void handleTankAdvert(const uint8_t *mac, const TankMeshAdvData &data) {
  int idx = findSlotByMac(mac);
  if (idx < 0) {
    if (tankCount >= MAX_TANKS) return;
    idx = tankCount++;
    memcpy(tanks[idx].mac, mac, 6);
    tanks[idx].inUse = true;
    tanks[idx].cachedSettingsVersion = 0xFE;
    tanks[idx].name[0] = '\0';
    tanks[idx].color = 0x808080;
  }
  TankSlot &t = tanks[idx];
  t.levelPercent = data.levelPercent;
  t.batteryMv = data.batteryMillivolts;
  t.counter = data.counter;
  t.advSettingsVersion = data.settingsVersion;
  t.lastSeenMs = millis();
  if (t.advSettingsVersion != t.cachedSettingsVersion) t.pendingFetch = true;
}

// ---------------- BLE scanning ----------------
BLEScan *pBLEScan;

class TankAdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (!advertisedDevice.haveManufacturerData()) return;
    String mfg = advertisedDevice.getManufacturerData();
    if (mfg.length() < 2 + sizeof(TankMeshAdvData)) return;
    uint16_t companyId = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
    if (companyId != TANKMESH_MFG_ID) return;
    TankMeshAdvData data;
    memcpy(&data, mfg.c_str() + 2, sizeof(data));
    if (data.magic0 != TANKMESH_MAGIC0 || data.magic1 != TANKMESH_MAGIC1) return;

    uint8_t mac[6];
    memcpy(mac, advertisedDevice.getAddress().getNative(), 6);
    handleTankAdvert(mac, data);
  }
};

// ---------------- BLE client actions (fetch / push settings) ----------------
BLEClient *bleClient = nullptr;

int findNextPendingAction() {
  unsigned long now = millis();
  for (int i = 0; i < tankCount; i++) {
    TankSlot &t = tanks[i];
    if ((t.pendingFetch || t.pendingWrite || t.pendingWriteCal || t.pendingWriteMesh) &&
        (now - t.lastAttemptMs > 15000)) return i;
  }
  return -1;
}

bool performBleAction(int idx) {
  TankSlot &t = tanks[idx];
  t.lastAttemptMs = millis();
  BLEAddress addr(t.mac);
  if (!bleClient) bleClient = BLEDevice::createClient();
  // connect(address, type, timeoutMS) — 0xFF lets the library auto-detect
  // address type, 3000ms bounds the worst case if the sensor isn't currently
  // in its connectable window. The 15s retry backoff in findNextPendingAction()
  // keeps failed attempts from repeating constantly.
  if (!bleClient->connect(addr, 0xFF, 3000)) return false;

  BLERemoteService *svc = bleClient->getService(TANKMESH_SERVICE_UUID);
  if (svc) {
    // Bundle every pending write into this one connection — catching a
    // sensor's short connectable window is the hard part, so once we're in,
    // apply everything queued for it rather than needing a fresh connection
    // per setting group.
    if (t.pendingWrite) {
      BLERemoteCharacteristic *nc = svc->getCharacteristic(TANKMESH_NAMECOLOR_UUID);
      if (nc) nc->writeValue((uint8_t *)&t.writeNameColor, sizeof(t.writeNameColor), true); // true = write with response
    }
    if (t.pendingWriteCal) {
      BLERemoteCharacteristic *cc = svc->getCharacteristic(TANKMESH_CAL_UUID);
      if (cc) cc->writeValue((uint8_t *)&t.writeCal, sizeof(t.writeCal), true);
    }
    if (t.pendingWriteMesh) {
      BLERemoteCharacteristic *mc = svc->getCharacteristic(TANKMESH_MESH_UUID);
      if (mc) mc->writeValue((uint8_t *)&t.writeMesh, sizeof(t.writeMesh), true);
    }

    BLERemoteCharacteristic *ncRead = svc->getCharacteristic(TANKMESH_NAMECOLOR_UUID);
    if (ncRead) {
      String v = ncRead->readValue();
      if (v.length() == sizeof(TankMeshNameColor)) {
        TankMeshNameColor nc;
        memcpy(&nc, v.c_str(), sizeof(nc));
        strncpy(t.name, nc.name, 15); t.name[15] = '\0';
        t.color = nc.colorRGB;
      }
    }
    BLERemoteCharacteristic *calRead = svc->getCharacteristic(TANKMESH_CAL_UUID);
    if (calRead) {
      String v = calRead->readValue();
      if (v.length() == sizeof(TankMeshCal)) memcpy(&t.cachedCal, v.c_str(), sizeof(t.cachedCal));
    }
    BLERemoteCharacteristic *meshRead = svc->getCharacteristic(TANKMESH_MESH_UUID);
    if (meshRead) {
      String v = meshRead->readValue();
      if (v.length() == sizeof(TankMeshMesh)) memcpy(&t.cachedMesh, v.c_str(), sizeof(t.cachedMesh));
    }
    t.calMeshKnown = true;

    BLERemoteCharacteristic *diagC = svc->getCharacteristic(TANKMESH_DIAG_UUID);
    if (diagC) {
      String v = diagC->readValue();
      if (v.length() == sizeof(TankMeshDiag)) {
        TankMeshDiag d;
        memcpy(&d, v.c_str(), sizeof(d));
        t.cachedSettingsVersion = d.settingsVersion;
      }
    }
  }
  bleClient->disconnect();
  t.pendingFetch = false;
  t.pendingWrite = false;
  t.pendingWriteCal = false;
  t.pendingWriteMesh = false;
  return true;
}

// ---------------- WiFi config portal ----------------
WebServer server(80);
Preferences prefs;
String apSSID = "TankMesh_Display", apPassword = "password123";

uint16_t textColorFor(uint32_t bgColor) {
  uint8_t r = (bgColor >> 16) & 0xFF, g = (bgColor >> 8) & 0xFF, b = bgColor & 0xFF;
  int luminance = (r * 299 + g * 587 + b * 114) / 1000;
  return luminance > 140 ? lcd.color888(0, 0, 0) : lcd.color888(255, 255, 255);
}

void handleRoot() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    apSSID = server.arg("ssid"); apSSID.trim();
    apPassword = server.arg("pass"); apPassword.trim();
    prefs.begin("mesh_disp", false);
    prefs.putString("ssid", apSSID);
    prefs.putString("pass", apPassword);
    prefs.end();
    server.send(200, "text/html", "<h3>Saved. Rebooting...</h3><script>setTimeout(()=>location.href='/',3000);</script>");
    delay(1000); ESP.restart(); return;
  }

  String h = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  h += "<style>body{font-family:sans-serif;background:#f4f6f9;padding:20px;}.c{background:white;padding:20px;border-radius:10px;max-width:600px;margin:0 auto 20px;}";
  h += "input[type=text],input[type=password],input[type=number]{padding:6px;margin:3px 0;}table{border-collapse:collapse;width:100%;margin-top:10px;}td,th{padding:8px;border-bottom:1px solid #eee;text-align:left;}";
  h += ".sw{display:inline-block;width:22px;height:22px;border-radius:4px;vertical-align:middle;border:1px solid #999;}";
  h += "fieldset{margin-bottom:14px;border-radius:8px;border:1px solid #ddd;} legend{font-weight:bold;padding:0 6px;}";
  h += ".row{margin:6px 0;} label{display:inline-block;min-width:130px;}</style></head><body>";

  h += "<div class='c'><h2>TankMesh Display Settings</h2><form method='POST'>SSID: <input type='text' name='ssid' value='" + apSSID + "'><br>Password: <input type='password' name='pass' value='" + apPassword + "'><br><input type='submit' value='Save & Reboot'></form></div>";

  h += "<div class='c'><h2>Tanks at a glance (" + String(tankCount) + "/" + String(MAX_TANKS) + ")</h2><table><tr><th>Swatch</th><th>Name</th><th>Level</th><th>Batt</th></tr>";
  for (int i = 0; i < tankCount; i++) {
    TankSlot &t = tanks[i];
    char hexColor[8]; snprintf(hexColor, sizeof(hexColor), "#%06X", t.color);
    h += "<tr><td><span class='sw' style='background:" + String(hexColor) + "'></span></td>";
    h += "<td>" + String(t.name[0] ? t.name : "(unassigned)") + "</td>";
    h += "<td>" + String(t.levelPercent) + "%</td>";
    h += "<td>" + (t.batteryMv > 0 ? String(t.batteryMv) + "mV" : String("-")) + "</td></tr>";
  }
  h += "</table></div>";

  h += "<div class='c'><h2>Tank Settings</h2>";
  for (int i = 0; i < tankCount; i++) {
    TankSlot &t = tanks[i];
    char hexColor[8]; snprintf(hexColor, sizeof(hexColor), "#%06X", t.color);
    h += "<fieldset><legend>" + macToStr(t.mac) + "</legend><form method='POST' action='/assign'>";
    h += "<input type='hidden' name='mac' value='" + macToStr(t.mac) + "'>";
    h += "<div class='row'><label>Name</label><input type='text' name='name' value='" + String(t.name) + "' maxlength='15'></div>";
    h += "<div class='row'><label>Color</label><input type='color' name='color' value='" + String(hexColor) + "'></div>";
    if (t.calMeshKnown) {
      h += "<div class='row'><label>Auto-calibrate</label><input type='checkbox' name='auto_cal'" + String(t.cachedCal.autoCalEnabled ? " checked" : "") + "></div>";
      h += "<div class='row'><label>Manual ADC min</label><input type='number' name='adc_min' value='" + String(t.cachedCal.adcMin) + "'></div>";
      h += "<div class='row'><label>Manual ADC max</label><input type='number' name='adc_max' value='" + String(t.cachedCal.adcMax) + "'></div>";
      h += "<div class='row'><label>Group label</label><input type='text' name='group' value='" + String(t.cachedMesh.prefix) + "' maxlength='15'></div>";
      h += "<div class='row'><label>Wake interval (sec)</label><input type='number' name='sleep_sec' value='" + String(t.cachedMesh.sleepIntervalSec) + "'></div>";
    } else {
      h += "<div class='row'><i>Calibration/mesh settings not fetched yet — reload this page in a few seconds.</i></div>";
    }
    h += "<input type='submit' value='Save All'></form></fieldset>";
  }
  h += "</div></body></html>";
  server.send(200, "text/html", h);
}

void handleAssign() {
  if (!server.hasArg("mac")) { server.sendHeader("Location", "/"); server.send(303); return; }
  uint8_t mac[6];
  if (!strToMac(server.arg("mac"), mac)) { server.sendHeader("Location", "/"); server.send(303); return; }
  int idx = findSlotByMac(mac);
  if (idx < 0) { server.sendHeader("Location", "/"); server.send(303); return; }
  TankSlot &t = tanks[idx];

  if (server.hasArg("name") && server.hasArg("color")) {
    String nm = server.arg("name"); nm.trim();
    strncpy(t.writeNameColor.name, nm.c_str(), 15);
    t.writeNameColor.name[15] = '\0';
    String colorStr = server.arg("color");
    if (colorStr.startsWith("#")) colorStr = colorStr.substring(1);
    t.writeNameColor.colorRGB = (uint32_t)strtol(colorStr.c_str(), NULL, 16);
    t.pendingWrite = true;
  }

  if (server.hasArg("adc_min") && server.hasArg("adc_max")) {
    t.writeCal.autoCalEnabled = server.hasArg("auto_cal") ? 1 : 0;
    t.writeCal.adcMin = (uint16_t)server.arg("adc_min").toInt();
    t.writeCal.adcMax = (uint16_t)server.arg("adc_max").toInt();
    t.pendingWriteCal = true;
  }

  if (server.hasArg("sleep_sec")) {
    String grp = server.hasArg("group") ? server.arg("group") : String(t.cachedMesh.prefix);
    grp.trim();
    strncpy(t.writeMesh.prefix, grp.c_str(), 15);
    t.writeMesh.prefix[15] = '\0';
    uint16_t sleepSec = (uint16_t)server.arg("sleep_sec").toInt();
    t.writeMesh.sleepIntervalSec = sleepSec < 5 ? 5 : sleepSec; // matches sensor-side safety floor
    t.pendingWriteMesh = true;
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

// ---------------- Drawing ----------------
int currentScreen = 0; // 0=Overview, 1..tankCount=tank detail, tankCount+1=Setup
unsigned long lastTapTime = 0;

void drawOverview() {
  lcd.fillScreen(lcd.color888(10, 10, 20));
  lcd.setTextColor(lcd.color888(255, 255, 255), lcd.color888(10, 10, 20));
  lcd.setFont(&fonts::Font4);
  lcd.drawCentreString("TankMesh - " + String(tankCount) + " tank" + (tankCount == 1 ? "" : "s"), SCREEN_W / 2, 10);

  int top = 45;
  int rowH = tankCount > 0 ? max(36, (SCREEN_H - top - 10) / tankCount) : 36;
  for (int i = 0; i < tankCount; i++) {
    TankSlot &t = tanks[i];
    int y = top + i * rowH;
    uint32_t swColor = lcd.color888((t.color >> 16) & 0xFF, (t.color >> 8) & 0xFF, t.color & 0xFF);
    lcd.fillRoundRect(10, y + 4, 26, rowH - 12, 4, swColor);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(lcd.color888(255, 255, 255), lcd.color888(10, 10, 20));
    String label = t.name[0] ? String(t.name) : ("(" + macToStr(t.mac).substring(9) + ")");
    lcd.drawString(label, 46, y + rowH / 2 - 8);
    lcd.drawRightString(String(t.levelPercent) + "%", SCREEN_W - 12, y + rowH / 2 - 8);
    lcd.drawFastHLine(10, y + rowH - 1, SCREEN_W - 20, lcd.color888(50, 50, 60));
  }
}

void drawTankDetail(int idx) {
  TankSlot &t = tanks[idx];
  uint32_t accent = lcd.color888((t.color >> 16) & 0xFF, (t.color >> 8) & 0xFF, t.color & 0xFF);
  uint32_t bg = lcd.color888(10, 10, 20);
  lcd.fillScreen(bg);
  lcd.setTextColor(accent, bg);
  lcd.setFont(&fonts::Font4);
  String label = t.name[0] ? String(t.name) : ("Unnamed " + macToStr(t.mac).substring(9));
  lcd.drawCentreString(label, SCREEN_W / 2, 15);

  int it = 70, ib = 340, il = SCREEN_W / 2 - 75, ir = SCREEN_W / 2 + 75;
  lcd.drawRect(il, it, ir - il, ib - it, lcd.color888(120, 120, 130));
  float lh = ((float)t.levelPercent / 100.0f) * (ib - it - 4);
  lcd.fillRect(il + 2, ib - 2 - (int)lh, ir - il - 4, (int)lh, accent);

  lcd.setTextColor(lcd.color888(255, 255, 255), bg);
  lcd.setFont(&fonts::Font7);
  lcd.drawCentreString(String(t.levelPercent), SCREEN_W / 2 - 15, 360);
  lcd.setFont(&fonts::Font4);
  lcd.drawString("%", SCREEN_W / 2 + 35, 375);

  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(lcd.color888(160, 160, 170), bg);
  String footer = "Packets: " + String(t.counter);
  if (t.batteryMv > 0) footer += "   Batt: " + String(t.batteryMv) + "mV";
  lcd.drawCentreString(footer, SCREEN_W / 2, 445);
}

void drawSetup() {
  uint32_t bg = lcd.color888(10, 10, 20);
  lcd.fillScreen(bg);
  lcd.setTextColor(lcd.color888(100, 200, 220), bg);
  lcd.setFont(&fonts::Font4);
  lcd.drawCentreString("SYSTEM SETUP", SCREEN_W / 2, 20);

  lcd.setTextColor(lcd.color888(255, 255, 255), bg);
  lcd.setFont(&fonts::Font2);
  int y = 70;
  lcd.drawString("WiFi SSID: " + apSSID, 20, y); y += 30;
  lcd.drawString("WiFi Pass: " + apPassword, 20, y); y += 30;
  lcd.drawString("Portal IP: 192.168.4.1", 20, y); y += 30;
  lcd.drawString("Tanks known: " + String(tankCount) + "/" + String(MAX_TANKS), 20, y); y += 40;

  int unassigned = 0;
  for (int i = 0; i < tankCount; i++) if (!tanks[i].name[0]) unassigned++;
  if (unassigned > 0) {
    lcd.setTextColor(lcd.color888(255, 180, 80), bg);
    lcd.drawString(String(unassigned) + " unassigned - name them via the WiFi portal:", 20, y); y += 26;
    lcd.setTextColor(lcd.color888(200, 200, 210), bg);
    for (int i = 0; i < tankCount && y < SCREEN_H - 20; i++) {
      if (!tanks[i].name[0]) { lcd.drawString(macToStr(tanks[i].mac), 30, y); y += 22; }
    }
  }
}

void updateUI() {
  static int lastScreen = -1;
  static uint32_t lastHash = 0xFFFFFFFF;
  uint32_t hash = tankCount;
  for (int i = 0; i < tankCount; i++) hash = hash * 31 + tanks[i].levelPercent + tanks[i].color + tanks[i].name[0];

  int maxScreen = tankCount + 1; // 0=overview .. tankCount=setup
  if (currentScreen > maxScreen) currentScreen = 0;

  if (currentScreen != lastScreen || hash != lastHash) {
    lastScreen = currentScreen; lastHash = hash;
    if (currentScreen == 0) drawOverview();
    else if (currentScreen <= tankCount) drawTankDetail(currentScreen - 1);
    else drawSetup();
  }
}

void checkTouchGestures() {
  int32_t touchX, touchY;
  if (lcd.getTouch(&touchX, &touchY)) {
    if (millis() - lastTapTime > 400) {
      lastTapTime = millis();
      int maxScreen = tankCount + 1;
      if (touchX > SCREEN_W / 2) currentScreen = (currentScreen - 1 + (maxScreen + 1)) % (maxScreen + 1);
      else currentScreen = (currentScreen + 1) % (maxScreen + 1);
    }
  }
}

// ---------------- BLE background task (core 0) ----------------
// All BLE setup AND scanning/connecting happens here, in one consistent
// task — some versions of this library are picky about the BLE stack being
// initialized and driven from the same task/core throughout, so init moved
// here rather than staying in setup().
void bleTaskFunc(void *param) {
  BLEDevice::init("TankMesh_MainDisplay");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new TankAdvCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(90);

  // WiFi AP needs to stay reachable for the config portal, and BLE scanning
  // tolerates missed cycles fine (advertisements just repeat), so bias the
  // shared radio toward WiFi rather than the default balanced split.
  // ESP_COEX_PREFER_WIFI was too aggressive — it starved BLE connection
  // attempts badly enough that settings writes routinely missed the
  // sensor's connectable window and only landed several retries later.
  // BALANCE is Espressif's own recommended default for mixed WiFi+BLE use.
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

  for (;;) {
    int idx = findNextPendingAction();
    if (idx >= 0) {
      performBleAction(idx);
    } else {
      pBLEScan->start(2, false);
      pBLEScan->clearResults();
      vTaskDelay(pdMS_TO_TICKS(500)); // idle gap so WiFi AP beacons aren't starved
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  lcd.init(); lcd.setRotation(0); lcd.setBrightness(140);
  lcd.fillScreen(lcd.color888(10, 10, 20));

  prefs.begin("mesh_disp", true);
  apSSID = prefs.getString("ssid", "TankMesh_Display");
  apPassword = prefs.getString("pass", "password123");
  prefs.end();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  server.on("/", handleRoot);
  server.on("/assign", HTTP_POST, handleAssign);
  server.begin();

  // Give the WiFi AP a clear run at finishing its own startup/calibration
  // before the BT controller initializes — bringing both up at once is a
  // known cause of the AP taking a long time to start beaconing reliably.
  delay(4000);

  xTaskCreatePinnedToCore(bleTaskFunc, "bleTask", 8192, NULL, 1, NULL, 0);

  updateUI();
}

void loop() {
  server.handleClient();
  checkTouchGestures();
  updateUI();
  delay(10);
}
