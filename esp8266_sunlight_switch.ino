// ===== File: astro_light_control.ino =====
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>

#define EEPROM_SIZE 256
#define SSID_ADDR 0
#define PASS_ADDR 100
#define SUNRISE_OFFSET_ADDR 140
#define SUNSET_OFFSET_ADDR 144
#define TZ_OFFSET_ADDR 148
#define LAT_ADDR 152
#define LON_ADDR 160
#define DST_MODE_ADDR 168  // 0=off, 1=manual, 2=auto
#define DST_MANUAL_ADDR 169 // 0=standard, 1=summer
#define RELAY_PIN 5

ESP8266WebServer server(80);
WiFiUDP ntpUDP;

int utcOffset = 3 * 3600;
int32_t sunriseOffsetMin = 0;
int32_t sunsetOffsetMin = 0;
float latitude = 46.4825;
float longitude = 30.7233;
uint8_t dstMode = 0;
uint8_t dstManual = 0;

String storedSSID;
String storedPASS;

time_t sunriseRaw = 0;
time_t sunsetRaw = 0;
time_t sunriseFinal = 0;
time_t sunsetFinal = 0;
time_t lastSyncTime = 0;
bool relayForced = false;

bool apMode = false;
unsigned long lastWiFiAttempt = 0;
const unsigned long wifiRetryInterval = 30UL * 60UL * 1000UL; // 30 minutes
int reconnectAttempts = 0;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 3UL * 60UL * 1000UL; // 3 minutes

NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffset, 3600 * 1000);

void saveWiFiToEEPROM(const String& ssid, const String& pass) {
  for (int i = 0; i < 32; i++) {
    EEPROM.write(SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
    EEPROM.write(PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  }
  EEPROM.commit();
}

void saveSettingsToEEPROM() {
  EEPROM.put(SUNRISE_OFFSET_ADDR, sunriseOffsetMin);
  EEPROM.put(SUNSET_OFFSET_ADDR, sunsetOffsetMin);
  EEPROM.put(TZ_OFFSET_ADDR, utcOffset);
  EEPROM.put(LAT_ADDR, latitude);
  EEPROM.put(LON_ADDR, longitude);
  EEPROM.write(DST_MODE_ADDR, dstMode);
  EEPROM.write(DST_MANUAL_ADDR, dstManual);
  EEPROM.commit();
}

void loadSettingsFromEEPROM() {
  char ssid[33], pass[33];
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(SSID_ADDR + i);
    pass[i] = EEPROM.read(PASS_ADDR + i);
  }
  ssid[32] = '\0';
  pass[32] = '\0';
  storedSSID = String(ssid);
  storedPASS = String(pass);

  EEPROM.get(SUNRISE_OFFSET_ADDR, sunriseOffsetMin);
  EEPROM.get(SUNSET_OFFSET_ADDR, sunsetOffsetMin);
  EEPROM.get(TZ_OFFSET_ADDR, utcOffset);
  EEPROM.get(LAT_ADDR, latitude);
  EEPROM.get(LON_ADDR, longitude);
  dstMode = EEPROM.read(DST_MODE_ADDR);
  dstManual = EEPROM.read(DST_MANUAL_ADDR);

  applyDST();
  timeClient.setTimeOffset(utcOffset);
}

void applyDST() {
  if (dstMode == 1 && dstManual == 1) {
    utcOffset += 3600;
  } else if (dstMode == 2) {
    // Simple DST rule: last Sunday of March to last Sunday of October
    time_t current = now();
    tmElements_t tm;
    breakTime(current, tm);
    if ((tm.Month > 3 && tm.Month < 10) ||
        (tm.Month == 3 && tm.Day >= 25 && weekday(current) == 1) ||
        (tm.Month == 10 && !(tm.Day >= 25 && weekday(current) == 1))) {
      utcOffset += 3600;
    }
  }
}

int getDayOfYear(time_t t) {
  tmElements_t tm;
  breakTime(t, tm);
  int day = tm.Day;
  for (int m = 1; m < tm.Month; m++) {
    if (m == 2) day += 28 + (year(t) % 4 == 0 ? 1 : 0);
    else if (m == 4 || m == 6 || m == 9 || m == 11) day += 30;
    else day += 31;
  }
  return day;
}

float calculateSolarEventUTC(bool isSunrise, float lat, float lon, int doy) {
  float zenith = 90.833;
  float D2R = PI / 180.0;
  float R2D = 180.0 / PI;
  float M = (0.9856 * doy) - 3.289;
  float L = fmod(M + 1.916 * sin(D2R * M) + 0.020 * sin(2 * D2R * M) + 282.634, 360);
  float RA = R2D * atan(0.91764 * tan(D2R * L));
  RA = fmod(RA + (floor(L / 90) * 90 - floor(RA / 90) * 90), 360) / 15;
  float sinDec = 0.39782 * sin(D2R * L);
  float cosDec = cos(asin(sinDec));
  float cosH = (cos(D2R * zenith) - (sinDec * sin(D2R * lat))) / (cosDec * cos(D2R * lat));
  if (cosH > 1 || cosH < -1) return -1;
  float H = isSunrise ? 360 - R2D * acos(cosH) : R2D * acos(cosH);
  H /= 15;
  float T = H + RA - 0.06571 * doy - 6.622;
  float UT = fmod((T - lon / 15.0) + 24, 24);
  return UT * 3600;
}

time_t getSunEventUTC(time_t now, bool isSunrise, float lat, float lon) {
  int doy = getDayOfYear(now);
  float utcSecs = calculateSolarEventUTC(isSunrise, lat, lon, doy);
  if (utcSecs < 0) return 0;
  tmElements_t tm;
  breakTime(now, tm);
  tm.Hour = 0; tm.Minute = 0; tm.Second = 0;
  return makeTime(tm) + (time_t)utcSecs;
}

void updateSunTimes() {
  time_t current = now();

  // Calculate sunrise and sunset in UTC first
  time_t sunriseUTC = getSunEventUTC(current, true, latitude, longitude);
  time_t sunsetUTC  = getSunEventUTC(current, false, latitude, longitude);

  // Convert to local time and apply user offsets
  sunriseRaw = sunriseUTC + utcOffset;
  sunsetRaw  = sunsetUTC + utcOffset;
  sunriseFinal = sunriseUTC + sunriseOffsetMin * 60 + utcOffset;
  sunsetFinal  = sunsetUTC  + sunsetOffsetMin * 60 + utcOffset;
}

String formatTime(time_t t) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour(t), minute(t), second(t));
  return String(buf);
}

String formatDelta(time_t t) {
  time_t current = now();
  time_t delta = current - t;
  int days = delta / 86400;
  int hours = (delta % 86400) / 3600;
  int mins = (delta % 3600) / 60;
  char buf[20];
  snprintf(buf, sizeof(buf), "%02dd:%02dh:%02dm", days, hours, mins);
  return String(buf);
}

void controlRelay(time_t now) {
  if (relayForced) {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Relay ON (forced)");
    return;
  }
  if (now >= sunsetFinal || now <= sunriseFinal) {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Relay ON");
  } else {
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Relay OFF");
  }
}

void handleRoot() {
  time_t current = now();
  bool relayState = digitalRead(RELAY_PIN) == LOW;
  String page = "<h1>ESP8266 Astro Light Control</h1>";
  page += "<p>Current time: " + formatTime(current) + "</p>";
  page += "<p>Time zone: UTC" + String((utcOffset >= 0 ? "+" : "")) + String(utcOffset / 3600) + "</p>";
  page += "<p>Last NTP sync: " + formatDelta(lastSyncTime) + " ago</p>";
  page += "<p><b>Relay state: " + String(relayState ? "ON" : "OFF") + (relayForced ? " (forced)" : "") + "</b></p>";
  page += "<hr>";
  page += "<p>Raw sunrise: " + formatTime(sunriseRaw) + "</p>";
  page += "<p>Raw sunset: " + formatTime(sunsetRaw) + "</p>";
  page += "<p>Final ON time: " + formatTime(sunsetFinal) + " (offset " + String(sunsetOffsetMin) + " min)</p>";
  page += "<p>Final OFF time: " + formatTime(sunriseFinal) + " (offset " + String(sunriseOffsetMin) + " min)</p>";
  page += "<hr>";
  page += "<p>Latitude: " + String(latitude, 6) + "</p>";
  page += "<p>Longitude: " + String(longitude, 6) + "</p>";
  page += "<p>DST Mode: " + String(dstMode == 0 ? "Off" : dstMode == 1 ? "Manual" : "Auto") + "</p>";
  if (dstMode == 1) page += "<p>DST Manual state: " + String(dstManual == 1 ? "Summer Time" : "Standard Time") + "</p>";
  page += "<hr>";
  page += R"rawliteral(
    <form method='POST' action='/save'>
      SSID:<br><input name='ssid'><br>
      Password:<br><input name='pass' type='password'><br><br>
      Timezone offset (hours):<br><input name='tz'><br>
      Sunrise offset (min):<br><input name='sunrise'><br>
      Sunset offset (min):<br><input name='sunset'><br>
      Latitude:<br><input name='lat'><br>
      Longitude:<br><input name='lon'><br>
      DST Mode (0=Off, 1=Manual, 2=Auto):<br><input name='dstmode'><br>
      If Manual, DST active? (0=Standard, 1=Summer):<br><input name='dstmanual'><br><br>
      <input type='submit' value='Save & Reboot'>
    </form>
    <form method='POST' action='/sync'>
      <input type='submit' value='Force NTP Sync'>
    </form>
    <form method='POST' action='/toggle'>
      <input type='submit' value='Toggle Relay State'>
    </form>
    <form method='POST' action='/auto'>
      <input type='submit' value='Enable Automatic Mode'>
    </form>
  )rawliteral";
  if (apMode) page += "<form method='POST' action='/connect'><input type='submit' value='Connect to Wi-Fi'></form>";
  page += R"rawliteral(
    <h3>Manual Time</h3>
    <button onclick='syncTime()'>Sync Browser Time</button>
    <form onsubmit='return setTime(event)'>
      <input type='datetime-local' id='manualTime'>
      <input type='submit' value='Set Time'>
    </form>
    <script>
    function syncTime(){
      const now = new Date();
      const epoch = Math.floor(now.getTime()/1000 - now.getTimezoneOffset()*60);
      fetch('/settime?epoch=' + epoch)
        .then(() => location.reload());
    }
    function setTime(e){
      e.preventDefault();
      const dt = document.getElementById('manualTime').value;
      if(!dt) return false;
      const d = new Date(dt);
      const epoch = Math.floor(d.getTime()/1000 - d.getTimezoneOffset()*60);
      fetch('/settime?epoch=' + epoch)
        .then(() => location.reload());
      return false;
    }
    </script>
  )rawliteral";
  server.send(200, "text/html", page);
}

void startWebInterface() {
  server.on("/", handleRoot);

  server.on("/save", []() {
    if (server.hasArg("ssid") && server.hasArg("pass") && server.arg("ssid") != "" && server.arg("pass") != "") {
      storedSSID = server.arg("ssid");
      storedPASS = server.arg("pass");
      saveWiFiToEEPROM(storedSSID, storedPASS);
    }
    if (server.hasArg("sunrise") && server.arg("sunrise") != "") sunriseOffsetMin = server.arg("sunrise").toInt();
    if (server.hasArg("sunset") && server.arg("sunset") != "")  sunsetOffsetMin  = server.arg("sunset").toInt();
    if (server.hasArg("tz") && server.arg("tz") != "") {
      utcOffset = server.arg("tz").toInt() * 3600;
    }
    if (server.hasArg("lat") && server.arg("lat") != "") latitude = server.arg("lat").toFloat();
    if (server.hasArg("lon") && server.arg("lon") != "") longitude = server.arg("lon").toFloat();
    if (server.hasArg("dstmode") && server.arg("dstmode") != "") dstMode = server.arg("dstmode").toInt();
    if (server.hasArg("dstmanual") && server.arg("dstmanual") != "") dstManual = server.arg("dstmanual").toInt();

    saveSettingsToEEPROM();
    server.send(200, "text/html", "<h1>Saved. Rebooting...</h1>");
    delay(1500);
    ESP.restart();
  });

  server.on("/sync", []() {
    if (timeClient.forceUpdate()) {
      time_t epoch = timeClient.getEpochTime();
      setTime(epoch);
      lastSyncTime = epoch;
      updateSunTimes();
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "Sync OK, redirecting...");
    } else {
      server.send(500, "text/plain", "Sync failed");
    }
  });

  server.on("/settime", []() {
    if (server.hasArg("epoch")) {
      time_t epoch = server.arg("epoch").toInt();
      setTime(epoch);
      lastSyncTime = epoch;
      updateSunTimes();
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "Time updated");
    } else {
      server.send(400, "text/plain", "Missing epoch");
    }
  });

  server.on("/toggle", []() {
    relayForced = !relayForced;
    controlRelay(now());
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Toggled");
  });

  server.on("/auto", []() {
    relayForced = false;
    controlRelay(now());
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Auto mode enabled");
  });

  server.on("/connect", []() {
    Serial.println("Manual Wi-Fi connect requested");
    lastWiFiAttempt = millis();
    attemptWiFiConnection();
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Connecting...");
  });

  server.begin();
}

void startAPMode() {
  Serial.println("Switching to Access Point mode");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("SunlightSetup");
  apMode = true;
  reconnectAttempts = 0;
  lastReconnectAttempt = 0;
  lastWiFiAttempt = millis();
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
}

bool attemptWiFiConnection() {
  if (apMode) {
    Serial.println("Attempting Wi-Fi connection while in AP mode");
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  WiFi.begin(storedSSID.c_str(), storedPASS.c_str());
  Serial.print("Connecting to Wi-Fi (SSID: ");
  Serial.print(storedSSID);
  Serial.print(")");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected: " + WiFi.localIP().toString());
    reconnectAttempts = 0;
    lastReconnectAttempt = 0;
    if (apMode) {
      Serial.println("Disabling AP mode");
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apMode = false;
    }
    return true;
  }
  Serial.println("\nWi-Fi connection failed");
  return false;
}

void setupWiFi() {
  Serial.println("Starting Wi-Fi setup");
  bool connected = false;
  for (int attempt = 0; attempt < 3 && !connected; attempt++) {
    Serial.print("Wi-Fi attempt ");
    Serial.println(attempt + 1);
    connected = attemptWiFiConnection();
    if (!connected && attempt < 2) {
      Serial.println("Retrying in 3 minutes...");
      delay(180000);
    }
  }
  if (!connected) {
    Serial.println("All attempts failed, starting AP mode");
    startAPMode();
  }
}

void setupOTA() {
  ArduinoOTA.setHostname("astro-light-control");
  ArduinoOTA.onStart([]() {
    Serial.println("OTA update started");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA update finished");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  EEPROM.begin(EEPROM_SIZE);
  loadSettingsFromEEPROM();
  setupWiFi();
  setupOTA();
  timeClient.setTimeOffset(utcOffset);
  timeClient.begin();
  if (timeClient.forceUpdate()) {
    time_t epoch = timeClient.getEpochTime();
    setTime(epoch);
    lastSyncTime = epoch;
  }
  updateSunTimes();
  startWebInterface();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  static unsigned long lastRelayCheck = 0;
  if (millis() - lastRelayCheck > 60000) {
    controlRelay(now());
    lastRelayCheck = millis();
  }

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    reconnectAttempts = 0;
    lastReconnectAttempt = 0;
  } else {
    if (!apMode) {
      if (reconnectAttempts < 3 &&
          (lastReconnectAttempt == 0 || millis() - lastReconnectAttempt >= reconnectInterval)) {
        Serial.print("Wi-Fi disconnected, retry ");
        Serial.println(reconnectAttempts + 1);
        lastReconnectAttempt = millis();
        if (!attemptWiFiConnection()) {
          reconnectAttempts++;
          if (reconnectAttempts >= 3) {
            Serial.println("Reconnection failed after 3 attempts, enabling AP mode");
            startAPMode();
          }
        }
      }
    } else if (millis() - lastWiFiAttempt > wifiRetryInterval) {
      Serial.println("AP mode: periodic Wi-Fi reconnect attempt");
      lastWiFiAttempt = millis();
      attemptWiFiConnection();
    }
  }
}
