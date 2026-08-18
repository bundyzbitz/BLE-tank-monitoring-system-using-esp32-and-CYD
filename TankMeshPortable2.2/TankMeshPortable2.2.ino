#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h> // Native I2C utility to parse hardware touch locations
#include <esp_now.h>

// 1. Optimized LovyanGFX parallel class exclusively focused on the ST7789 display lines
class LGFX_CYD_22P : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_Parallel8 _bus_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX_CYD_22P() {
    {
      auto cfg = _bus_instance.config();
      cfg.freq_write = 16000000;
      cfg.pin_wr = 4;
      cfg.pin_rd = 2;
      cfg.pin_rs = 16;
      // Data bus pin mappings (D0 - D7)
      cfg.pin_d0 = 15;
      cfg.pin_d1 = 13;
      cfg.pin_d2 = 12;
      cfg.pin_d3 = 14;
      cfg.pin_d4 = 27;
      cfg.pin_d5 = 25;
      cfg.pin_d6 = 33;
      cfg.pin_d7 = 32;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 17;
      cfg.pin_rst = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 0;
      cfg.freq = 12000;
      cfg.pwm_channel = 1;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX_CYD_22P lcd;

// Expanded matching packet structure layout to ensure packet alignment
struct __attribute__((packed)) TankMeshPacket {
  char prefix[16];
  uint8_t sensorIndex;
  uint16_t analogValue;
  uint32_t counter;
  char sensorName[16]; // Added custom name string variable property
};

// Global Server & Database Variables
WebServer server(80);
Preferences prefs;
uint16_t tankLevels[4] = {0, 0, 0, 0};
uint32_t tankCounters[4] = {0, 0, 0, 0};
String receivedNames[4] = {"", "Loading...", "Loading...", "Loading..."}; // Custom runtime layout buffer arrays
String expectedPrefix = "TankMesh";
int currentScreen = 0; // 0=Tank1, 1=Tank2, 2=Tank3, 3=Settings

// Debounce timer tracking for touch presses
unsigned long lastTapTime = 0;

void handleRoot() {
  if (server.hasArg("prefix")) {
    String newPrefix = server.arg("prefix");
    newPrefix.trim();
    if (newPrefix.length() > 0 && newPrefix.length() < 16) {
      expectedPrefix = newPrefix;
      prefs.begin("mesh_port", false);
      prefs.putString("prefix", expectedPrefix);
      prefs.end();

      String redirectHtml = "<html><body style='font-family:Arial;text-align:center;padding:50px;'><h2>Prefix Changed!</h2>";
      redirectHtml += "<script>setTimeout(() => { location.href = '/'; }, 1000);</script></body></html>";
      server.send(200, "text/html", redirectHtml);
      return;
    }
  }

  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'></head><body style='font-family:sans-serif;padding:20px;'>";
  html += "<h2>TankMesh 2.2 Dashboard Portal</h2>";
  html += "<form method='POST'>Filter Prefix: <input type='text' name='prefix' value='" + expectedPrefix + "' maxlength='15'> <input type='submit' value='Save'></form><br>";
  html += "<table border='1' cellpadding='8' style='border-collapse:collapse;width:100%;'>";
  html += "<tr style='background:#ddd;'><th>Tank Name</th><th>Data Value</th><th>Packet Count</th></tr>";
  for (int i = 1; i <= 3; i++) {
    html += "<tr><td>" + receivedNames[i] + " (Index " + String(i) + ")</td><td><b>" + String(tankLevels[i]) + "%</b></td><td>" + String(tankCounters[i]) + "</td></tr>";
  }
  html += "</table></body></html>";
  server.send(200, "text/html", html);
}

void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *incomingData, int len) {
  if (len == sizeof(TankMeshPacket)) {
    TankMeshPacket *pkt = (TankMeshPacket *)incomingData;
    if (String(pkt->prefix) == expectedPrefix) {
      uint8_t idx = pkt->sensorIndex;
      if (idx >= 1 && idx <= 3) {
        tankLevels[idx] = pkt->analogValue;
        tankCounters[idx] = pkt->counter;
        
        // Dynamic string parsing sequence logic
        char safeName[17];
        memset(safeName, 0, sizeof(safeName));
        memcpy(safeName, pkt->sensorName, 16);
        receivedNames[idx] = String(safeName);
      }
    }
  }
}

void drawSingleTankView(int tankId, uint32_t bg_color, uint32_t txt_color) {
  int itop = 60;
  int ibottom = 250;
  int ileft = 120 - 60;
  int iright = 120 + 60;
  float lheight;

  lcd.fillScreen(bg_color);
  lcd.setTextColor(txt_color, bg_color);
  lcd.setFont(&fonts::Font4);
  
  // Replaced original fixed hardcoded array setup with live custom transmitted network properties
  lcd.drawCentreString(receivedNames[tankId], 120, 10);

  lcd.drawLine(ileft, itop, ileft, ibottom, 0x000000);
  lcd.drawLine(ileft + 1, itop, ileft + 1, ibottom, 0x000000);
  lcd.drawLine(ileft + 2, itop, ileft + 2, ibottom, 0x000000);
  lcd.drawLine(iright, itop, iright, ibottom, 0x000000);
  lcd.drawLine(iright - 1, itop, iright - 1, ibottom, 0x000000);
  lcd.drawLine(iright - 2, itop, iright - 2, ibottom, 0x000000);
  lcd.drawLine(ileft, ibottom, iright, ibottom, 0x000000);
  lcd.drawLine(ileft, ibottom - 1, iright, ibottom - 1, 0x000000);
  lcd.drawLine(ileft, ibottom - 2, iright, ibottom - 2, 0x000000);

  lheight = 0.0 - ((float)tankLevels[tankId] / 100.0) * (ibottom - 2.0 - itop);
  lcd.fillRect(ileft + 3, ibottom - 2, iright - ileft - 5, lheight, 0x0000FF);

  lcd.setFont(&fonts::Font7);
  lcd.drawCentreString(String(tankLevels[tankId]), 120, 260);
  lcd.setFont(&fonts::Font4);
  lcd.drawString("%", 155, 260);
}

void drawSettingsView() {
  lcd.fillScreen(0x000000);
  lcd.setTextColor(0xFFFFFF, 0x000000);
  lcd.setFont(&fonts::Font4);
  lcd.drawCentreString("SYSTEM SETUP", 120, 30);

  lcd.setFont(&fonts::Font2);
  lcd.drawString("Prefix: " + expectedPrefix, 15, 120);
  lcd.drawString("IP: 192.168.4.1", 15, 160);
  lcd.drawString("SSID: TankMesh_Portable", 15, 200);
  lcd.drawString("PASSWORD: 12345678", 15, 240);
}

void updateUI() {
  static int lastScreen = -1;
  static uint16_t lastVals[4] = {9999, 9999, 9999, 9999};
  static String lastNames[4] = {"", "", "", ""};

  // Re-evaluation matrix includes string transitions to catch name modifications dynamically
  bool dataChanged = (tankLevels[1] != lastVals[1] || tankLevels[2] != lastVals[2] || tankLevels[3] != lastVals[3] ||
                      receivedNames[1] != lastNames[1] || receivedNames[2] != lastNames[2] || receivedNames[3] != lastNames[3]);

  if (currentScreen != lastScreen || dataChanged) {
    lastScreen = currentScreen;
    for (int i = 1; i <= 3; i++) {
      lastVals[i] = tankLevels[i];
      lastNames[i] = receivedNames[i];
    }

    switch (currentScreen) {
    case 0: drawSingleTankView(1, 0xFF0000, 0xFFFFFF); break; // Red Background
    case 1: drawSingleTankView(2, 0x00FF00, 0x000000); break; // Green Background
    case 2: drawSingleTankView(3, 0xFFFF00, 0x000000); break; // Yellow Background
    case 3: drawSettingsView(); break;
    }
  }
}

void checkTouchGestures() {
  if (millis() - lastTapTime < 350) return; // Ignore bouncing touch signals

  Wire.beginTransmission(0x15);
  if (Wire.endTransmission() != 0) return; 

  Wire.beginTransmission(0x15);
  Wire.write(0x02);
  if (Wire.endTransmission() == 0) {
    Wire.requestFrom(0x15, 6);
    if (Wire.available() >= 6) {
      uint8_t touchPoints = Wire.read() & 0x0F;
      uint8_t xHi = Wire.read();
      uint8_t xLo = Wire.read();

      if (touchPoints > 0 && touchPoints <= 5) {
        int16_t touchX = ((xHi & 0x0F) << 8) | xLo;
        if (touchX > 0 && touchX < 240) {
          lastTapTime = millis(); 
          if (touchX < 120) {
            currentScreen = (currentScreen - 1 + 4) % 4;
          } else {
            currentScreen = (currentScreen + 1) % 4;
          }
        }
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22); // Fire up standard I2C channels mapping
  
  lcd.init();
  lcd.setRotation(0);
  
  prefs.begin("mesh_port", true);
  expectedPrefix = prefs.getString("prefix", "TankMesh");
  prefs.end();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("TankMesh_Portable", "12345678");

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Error");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);

  server.on("/", handleRoot);
  server.begin();
  
  // Force clean immediate display paint on boot configuration pipelines
  updateUI(); 
}

void loop() {
  server.handleClient();
  checkTouchGestures();
  updateUI();
  delay(10);
}
