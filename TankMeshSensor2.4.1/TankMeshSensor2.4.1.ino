#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Update.h>

#define ANALOG_PIN 36 
#define TX_INTERVAL_MS 4000 

const float ALPHA = 0.15; 
WebServer server(80);
Preferences prefs;

struct __attribute__((packed)) TankMeshPacket {
  char prefix[16];      
  uint8_t sensorIndex;  
  uint16_t analogValue; 
  uint32_t counter;     
  char sensorName[16];  // Added to transmit the human-readable name to receiver screens
} txPacket;

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
unsigned long lastTxTime = 0;
String espNowLog = "";
String apPassword = "12345678";

uint16_t adcMin = 4095, adcMax = 0;
bool isCalibrated = false, autoCalEnabled = true; 
float filteredAdc = -1.0; 

void handleRoot() {
  if (server.hasArg("reset_cal")) {
    prefs.begin("tank_cal", false);
    prefs.putBool("is_cal", false);
    prefs.putUInt("adc_min", 4095);
    prefs.putUInt("adc_max", 0);
    prefs.end();
    adcMin = 4095; adcMax = 0; isCalibrated = false; filteredAdc = -1.0;
    server.send(200, "text/html", "<h3>Resetting...</h3><script>setTimeout(()=>location.href='/',2000);</script>");
    return;
  }

  if (server.hasArg("prefix") && server.hasArg("idx")) {
    String newPrefix = server.arg("prefix");
    uint8_t newIdx = server.arg("idx").toInt();
    String newApPassword = server.arg("ap_pass");
    String newName = server.arg("s_name"); // Extract sensor name from form
    if (newApPassword.length() < 8) newApPassword = "12345678";
    
    bool newAutoCal = server.hasArg("auto_cal");
    uint16_t newMin = server.arg("manual_min").toInt();
    uint16_t newMax = server.arg("manual_max").toInt();

    prefs.begin("tankmesh", false);
    prefs.putString("prefix", newPrefix);
    prefs.putUInt("idx", newIdx);
    prefs.putString("ap_pass", newApPassword);
    prefs.putString("s_name", newName); // Save the name to non-volatile flash storage
    prefs.end();

    prefs.begin("tank_cal", false);
    prefs.putBool("auto_cal", newAutoCal);
    prefs.putUInt("adc_min", newMin);
    prefs.putUInt("adc_max", newMax);
    if (!newAutoCal) prefs.putBool("is_cal", true);
    prefs.end();

    server.send(200, "text/html", "<h3>Saved. Rebooting...</h3><script>setTimeout(()=>location.href='/',2000);</script>");
    delay(1000); ESP.restart(); return;
  }

  uint16_t liveRawAdc = analogRead(ANALOG_PIN);
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif;background:#f4f6f9;padding:20px;color:#333;} ";
  html += ".card{background:white;padding:20px;border-radius:10px;max-width:500px;margin:0 auto;box-shadow:0 4px 10px rgba(0,0,0,0.05);} ";
  html += "h2{color:#1a365d;border-bottom:1px solid #ddd;padding-bottom:5px;} ";
  html += "input[type='text'],input[type='number'],input[type='password'],input[type='file']{width:90%;padding:8px;margin:10px 0;border:1px solid #ccc;border-radius:4px;} ";
  html += "input[type='submit']{background:#3182ce;color:white;border:none;padding:10px 15px;border-radius:4px;cursor:pointer;font-weight:bold;} ";
  html += ".btn-danger{background:#e53e3e!important;} pre{background:#eee;padding:10px;border-radius:5px;overflow-x:auto;} ";
  html += ".cb{margin:15px 0;font-weight:bold;} .ota-box{background:#ebf8ff;padding:12px;border-radius:6px;border:1px solid #bee3f8;margin-bottom:15px;}</style></head><body><div class='card'>";
  html += "<h2>TankMesh Config</h2>";
  
  // OTA Update Form
  html += "<div class='ota-box'><b>Firmware OTA Update (.bin):</b>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin'>";
  html += "<input type='submit' value='Upload & Flash' style='background:#2b6cb0;'></form></div>";

  html += "<p><b>Sensor Name Broadcast:</b> " + String(txPacket.sensorName) + "</p>";
  html += "<p><b>Scaled Sent:</b> " + String(txPacket.analogValue) + "%</p>";
  html += "<p><b>Raw ADC:</b> " + String(liveRawAdc) + "</p>";
  html += "<p><b>Filtered EMA:</b> " + String((int)filteredAdc) + "</p>";
  html += "<p><b>Limits:</b> Min=" + String(adcMin) + " | Max=" + String(adcMax) + "</p>";
  html += "<p><b>Auto-Cal:</b> " + String(autoCalEnabled ? "ON" : "OFF") + "</p><form method='POST'>";
  html += "AP Password:<input type='password' id='ap_pass' name='ap_pass' value='" + apPassword + "' minlength='8'><br>";
  html += "<div class='cb'><input type='checkbox' id='tp' onclick='tP()'><label for='tp'> Show Password</label></div><hr>";
  
  // Custom Text Field for User to Name the Sensor
  html += "Sensor Name (e.g. Front Tank):<input type='text' name='s_name' value='" + String(txPacket.sensorName) + "' maxlength='15'><br>";
  html += "Mesh Name:<input type='text' name='prefix' value='" + String(txPacket.prefix) + "' maxlength='15'><br>";
  html += "Sensor Index:<input type='number' name='idx' value='" + String(txPacket.sensorIndex) + "' min='1' max='3'><br><hr>";
  html += "<div class='cb'><input type='checkbox' id='ac' name='auto_cal' value='1' " + String(autoCalEnabled ? "checked" : "") + "><label for='ac'> Enable Auto-Cal</label></div>";
  html += "Manual Min:<input type='number' name='manual_min' value='" + String(adcMin) + "'><br>";
  html += "Manual Max:<input type='number' name='manual_max' value='" + String(adcMax) + "'><br><br>";
  html += "<input type='submit' value='Save Settings'></form><hr>";
  html += "<form method='POST'><input type='hidden' name='reset_cal' value='1'><input type='submit' class='btn-danger' value='Reset Auto-Cal Loop'></form>";
  html += "<h3>ESP-NOW Log:</h3><pre>" + espNowLog + "</pre></div>";
  html += "<script>function tP(){var x=document.getElementById('ap_pass');x.type=x.type==='password'?'text':'password';}</script></body></html>";
  server.send(200, "text/html", html);
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("Update Success: %u bytes\nRebooting...\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void onDataSent(const wifi_tx_info_t *txInfo, esp_now_send_status_t status) {
  espNowLog = "[" + String(millis()) + "] Cnt: " + String(txPacket.counter) + " | Status: " + (status == ESP_NOW_SEND_SUCCESS ? "OK" : "Fail") + "\n" + espNowLog;
  if (espNowLog.length() > 1000) espNowLog = espNowLog.substring(0, 1000);
}

void setup() {
  Serial.begin(115200);
  analogSetAttenuation(ADC_11db); 

  prefs.begin("tankmesh", true);
  String storedPrefix = prefs.getString("prefix", "TankMesh");
  uint8_t storedIdx = prefs.getUInt("idx", 1);
  apPassword = prefs.getString("ap_pass", "12345678");
  String storedName = prefs.getString("s_name", "SensorNode"); // Load the user-configured name
  prefs.end();

  memset(txPacket.prefix, 0, sizeof(txPacket.prefix));
  storedPrefix.toCharArray(txPacket.prefix, 16);
  txPacket.sensorIndex = storedIdx;
  txPacket.counter = 0;
  
  // Clear and copy the custom name string into the network structure array
  memset(txPacket.sensorName, 0, sizeof(txPacket.sensorName));
  storedName.toCharArray(txPacket.sensorName, 16);

  prefs.begin("tank_cal", true);
  isCalibrated = prefs.getBool("is_cal", false);
  autoCalEnabled = prefs.getBool("auto_cal", true); 
  adcMin = prefs.getUInt("adc_min", 4095);
  adcMax = prefs.getUInt("adc_max", 0);
  prefs.end();

  WiFi.mode(WIFI_AP_STA);
  String apName = "TankMesh_Sensor_" + String(txPacket.sensorIndex);
  WiFi.softAP(apName.c_str(), apPassword.c_str());

  if (esp_now_init() != ESP_OK) return;
  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  if (esp_now_add_peer(&peerInfo) != ESP_OK) Serial.println("Peer Fail");

  server.on("/", handleRoot);
  
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", "<h3>Update Finished! Rebooting node...</h3><script>setTimeout(()=>location.href='/',5000);</script>");
    delay(1000);
    ESP.restart();
  }, handleUpload);

  server.begin();
}

void loop() {
  server.handleClient();

  if (millis() - lastTxTime >= TX_INTERVAL_MS) {
    lastTxTime = millis();
    uint16_t rawAdc = analogRead(ANALOG_PIN);

    if (filteredAdc < 0) {
      filteredAdc = (float)rawAdc; 
    } else {
      filteredAdc = (ALPHA * (float)rawAdc) + ((1.0 - ALPHA) * filteredAdc);
    }

    uint16_t workingAdc = (uint16_t)filteredAdc;
        Serial.println(String(millis()) + " Min:" + adcMin + " Max:" + adcMax + " Raw:" + rawAdc + " Filtered:" + workingAdc);

    if (autoCalEnabled) {
      bool limitsChanged = false;
      if (!isCalibrated) {
        adcMin = workingAdc; adcMax = workingAdc; isCalibrated = true; limitsChanged = true;
      } else {
        if (workingAdc < adcMin) { adcMin = workingAdc; limitsChanged = true; }
        if (workingAdc > adcMax) { adcMax = workingAdc; limitsChanged = true; }
      }

      if (limitsChanged) {
        prefs.begin("tank_cal", false);
        prefs.putBool("is_cal", isCalibrated);
        prefs.putUInt("adc_min", adcMin);
        prefs.putUInt("adc_max", adcMax);
        prefs.end();
      }
    }

    if (adcMax > adcMin) {
      uint32_t tempValue = ((uint32_t)(workingAdc - adcMin) * 100) / (adcMax - adcMin);
      txPacket.analogValue = (uint16_t)constrain(tempValue, 0, 100);
    } else {
      txPacket.analogValue = 0;
    }

    txPacket.counter++;
    esp_now_send(broadcastAddress, (uint8_t *)&txPacket, sizeof(txPacket));
  }
}
