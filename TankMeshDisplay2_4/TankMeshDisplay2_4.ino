#define LGFX_USE_V1
#include <LovyanGFX.hpp> 
#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

const float ALPHA = 0.15;
#define SERVICE_UUID "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define CHARACTERISTIC_UUID "BEA5CE10-E51B-4A33-B1EE-050E278A6420"
BLECharacteristic *pCh;
bool bConn = false;

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pS) { bConn = true; };
  void onDisconnect(BLEServer *pS) { bConn = false; BLEDevice::startAdvertising(); }
};

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

struct __attribute__((packed)) TankMeshPacket {
  char prefix[16]; uint8_t sensorIndex; uint16_t analogValue; uint32_t counter; char sensorName[16];
};

WebServer server(80); Preferences prefs;
uint16_t tankLevels[4] = {0, 0, 0, 0}; uint32_t tankCounters[4] = {0, 0, 0, 0};
float filteredTankLevels[4] = {-1.0, -1.0, -1.0, -1.0};
String receivedNames[4] = {"", "Loading T1...", "Loading T2...", "Loading T3..."};
String expectedPrefix = "TankMesh", apSSID = "TankMesh_Display", apPassword = "password123";
int currentScreen = 0; unsigned long lastTapTime = 0;

void handleRoot() {
  if (server.hasArg("prefix") && server.hasArg("ssid") && server.hasArg("pass")) {
    expectedPrefix = server.arg("prefix"); expectedPrefix.trim();
    apSSID = server.arg("ssid"); apSSID.trim(); apPassword = server.arg("pass"); apPassword.trim();
    prefs.begin("mesh_disp", false); prefs.putString("prefix", expectedPrefix);
    prefs.putString("ssid", apSSID); prefs.putString("pass", apPassword); prefs.end();
    server.send(200, "text/html", "<h3>Saved. Rebooting...</h3><script>setTimeout(()=>location.href='/',3000);</script>");
    delay(1000); ESP.restart(); return;
  }
  String h = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  h += "<style>body{font-family:sans-serif;background:#f4f6f9;padding:20px;}.c{background:white;padding:20px;border-radius:10px;max-width:450px;margin:0 auto;}input{width:94%;padding:10px;margin:5px 0;}table{border-collapse:collapse;width:100%;margin-top:15px;}td,th{padding:10px;border-bottom:1px solid #eee;}</style></head><body>";
  h += "<div class='c'><h2>Display Settings</h2><form method='POST'>Prefix: <input type='text' name='prefix' value='" + expectedPrefix + "'><br>SSID: <input type='text' name='ssid' value='" + apSSID + "'><br>Password: <input type='password' name='pass' value='" + apPassword + "'><br><input type='submit' value='Save & Reboot'></form><h2>Live Values</h2><table>";
  for (int i = 1; i <= 3; i++) h += "<tr><td>" + receivedNames[i] + "</td><td><b>" + String(tankLevels[i]) + "%</b></td></tr>";
  h += "</table></div></body></html>"; server.send(200, "text/html", h);
}

void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *incomingData, int len) {
  if (len == sizeof(TankMeshPacket)) {
    TankMeshPacket *pkt = (TankMeshPacket *)incomingData;
    if (String(pkt->prefix) == expectedPrefix) {
      uint8_t idx = pkt->sensorIndex;
      if (idx >= 1 && idx <= 3) {
        if (filteredTankLevels[idx] < 0) filteredTankLevels[idx] = (float)pkt->analogValue;
        else filteredTankLevels[idx] = (ALPHA * (float)pkt->analogValue) + ((1.0 - ALPHA) * filteredTankLevels[idx]);
        tankLevels[idx] = (uint16_t)(filteredTankLevels[idx] + 0.5); tankCounters[idx] = pkt->counter;
        char safeName[17]; memset(safeName, 0, sizeof(safeName)); memcpy(safeName, pkt->sensorName, 16); receivedNames[idx] = String(safeName);
        
        // FIXED: Transmit simple single-tank packets instantly ("INDEX:NAME|LEVEL")
        if (bConn) {
          String bStr = String(idx) + ":" + receivedNames[idx] + "|" + String(tankLevels[idx]);
          pCh->setValue(bStr.c_str()); pCh->notify();
        }
      }
    }
  }
}

void drawMultiView() {
  uint32_t bk = lcd.color888(0, 0, 0), rd = lcd.color888(255, 0, 0), gr = lcd.color888(0, 255, 0), yl = lcd.color888(255, 255, 0), wt = lcd.color888(255, 255, 255);
  lcd.fillScreen(bk);
  lcd.fillRect(0, 0, 158, 238, rd); lcd.setTextColor(wt, rd); lcd.setFont(&fonts::Font4); lcd.drawCentreString(receivedNames[1], 79, 30); lcd.setFont(&fonts::Font7); lcd.drawCentreString(String(tankLevels[1]) + "%", 79, 100);
  lcd.fillRect(162, 0, 158, 238, gr); lcd.setTextColor(bk, gr); lcd.setFont(&fonts::Font4); lcd.drawCentreString(receivedNames[2], 240, 30); lcd.setFont(&fonts::Font7); lcd.drawCentreString(String(tankLevels[2]) + "%", 240, 100);
  lcd.fillRect(0, 242, 320, 238, yl); lcd.setTextColor(bk, yl); lcd.setFont(&fonts::Font4); lcd.drawCentreString(receivedNames[3] + " STATUS", 160, 260); lcd.setFont(&fonts::Font7); lcd.drawCentreString(String(tankLevels[3]) + "%", 160, 330);
}

void drawSingleTankView(int tankId, uint32_t bg_color, uint32_t txt_color) {
  int it = 70, ib = 340, il = 160 - 75, ir = 160 + 75; float lh;
  lcd.fillScreen(bg_color); lcd.setTextColor(txt_color, bg_color); lcd.setFont(&fonts::Font4); lcd.drawCentreString(receivedNames[tankId], 160, 15);
  lcd.drawLine(il, it, il, ib, 0); lcd.drawLine(il + 1, it, il + 1, ib, 0); lcd.drawLine(il + 2, it, il + 2, ib, 0); lcd.drawLine(ir, it, ir, ib, 0); lcd.drawLine(ir - 1, it, ir - 1, ib, 0); lcd.drawLine(ir - 2, it, ir - 2, ib, 0);
  lcd.drawLine(il, ib, ir, ib, 0); lcd.drawLine(il, ib - 1, ir, ib - 1, 0); lcd.drawLine(il, ib - 2, ir, ib - 2, 0);
  lh = 0.0 - ((float)tankLevels[tankId] / 100.0) * (ib - 3.0 - it); lcd.fillRect(il + 3, ib - 2, ir - il - 5, lh, lcd.color888(0, 0, 255));
  lcd.setFont(&fonts::Font7); lcd.drawCentreString(String(tankLevels[tankId]), 145, 365); lcd.setFont(&fonts::Font4); lcd.drawString("%", 195, 365);
  lcd.setFont(&fonts::Font2); lcd.drawCentreString("Total Packets: " + String(tankCounters[tankId]), 160, 445);
}

void drawSettingsView() {
  uint32_t cb = lcd.color888(0, 0, 0), cw = lcd.color888(255, 255, 255); lcd.fillScreen(cb); lcd.setTextColor(cw, cb); lcd.setFont(&fonts::Font4); lcd.drawCentreString("SYSTEM SETUP", 160, 40); lcd.setFont(&fonts::Font2); lcd.setTextSize(1.2);
  lcd.drawString("Prefix: " + expectedPrefix, 25, 140); lcd.drawString("IP: 192.168.4.1", 25, 200); lcd.drawString("SSID: " + apSSID, 25, 260); lcd.drawString("PASSWORD: " + apPassword, 25, 320); lcd.setTextSize(1.0);
}

void updateUI() {
  static int lastScreen = -1; static uint16_t lastVals[4] = {9999, 9999, 9999, 9999}; static String lastNames[4] = {"", "", "", ""};
  bool dataChanged = (tankLevels[1] != lastVals[1] || tankLevels[2] != lastVals[2] || tankLevels[3] != lastVals[3] || receivedNames[1] != lastNames[1] || receivedNames[2] != lastNames[2] || receivedNames[3] != lastNames[3]);
  if (currentScreen != lastScreen || dataChanged) {
    lastScreen = currentScreen; for (int i = 1; i <= 3; i++) { lastVals[i] = tankLevels[i]; lastNames[i] = receivedNames[i]; }
    uint32_t cr = lcd.color888(255, 0, 0), cw = lcd.color888(255, 255, 255), cg = lcd.color888(0, 255, 0), cb = lcd.color888(0, 0, 0), cy = lcd.color888(255, 255, 0);
    switch (currentScreen) {
      case 0: drawMultiView(); break; case 1: drawSingleTankView(1, cr, cw); break; case 2: drawSingleTankView(2, cg, cb); break; case 3: drawSingleTankView(3, cy, cb); break; case 4: drawSettingsView(); break;
    }
  }
}

void checkTouchGestures() {
  int32_t touchX, touchY; if (lcd.getTouch(&touchX, &touchY)) { if (millis() - lastTapTime > 400) { lastTapTime = millis(); if (touchX > 160) currentScreen = (currentScreen - 1 + 5) % 5; else currentScreen = (currentScreen + 1) % 5; updateUI(); } }
}

void setup() {
  Serial.begin(115200); lcd.init(); lcd.setRotation(0); lcd.setBrightness(140); lcd.fillScreen(lcd.color888(0, 0, 0));
  prefs.begin("mesh_disp", true); expectedPrefix = prefs.getString("prefix", "TankMesh"); apSSID = prefs.getString("ssid", "TankMesh_Display"); apPassword = prefs.getString("pass", "password123"); prefs.end();
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  BLEDevice::init("TankMesh_Gateway"); BLEServer *pServer = BLEDevice::createServer(); pServer->setCallbacks(new MyServerCallbacks()); BLEService *pService = pServer->createService(SERVICE_UUID);
  pCh = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY); pCh->addDescriptor(new BLE2902()); pService->start(); BLEDevice::getAdvertising()->start();
  if (esp_now_init() != ESP_OK) return; esp_now_register_recv_cb(onDataRecv); server.on("/", handleRoot); server.begin(); updateUI();
}

void loop() { server.handleClient(); checkTouchGestures(); updateUI(); delay(15); }
