// ===== File: astro_light_control.ino =====
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <EEPROM.h>

#define EEPROM_SIZE 256
#define SSID_ADDR 0
#define PASS_ADDR 100
#define SUNRISE_OFFSET_ADDR 140
#define SUNSET_OFFSET_ADDR 144
#define TZ_OFFSET_ADDR 148
#define LAT_ADDR 152
#define LON_ADDR 160
#define DST_MODE_ADDR 168
#define DST_ACTIVE_ADDR 169

ESP8266WebServer server(80);
WiFiUDP ntpUDP;
int utcOffset = 3 * 3600;
int32_t sunriseOffsetMin = 0;
int32_t sunsetOffsetMin = 0;
float latitude = 46.4825;
float longitude = 30.7233;
uint8_t dstMode = 0;         // 0 = Off, 1 = Manual, 2 = Auto (future)
uint8_t dstActiveManual = 0; // 0 = standard, 1 = summer
String storedSSID;
String storedPASS;
time_t sunriseRaw = 0;
time_t sunsetRaw = 0;
time_t sunriseFinal = 0;
time_t sunsetFinal = 0;
time_t lastSyncTime = 0;
bool relayForced = false;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffset, 3600 * 1000);

void saveSettingsToEEPROM() {
  EEPROM.put(SUNRISE_OFFSET_ADDR, sunriseOffsetMin);
  EEPROM.put(SUNSET_OFFSET_ADDR, sunsetOffsetMin);
  EEPROM.put(TZ_OFFSET_ADDR, utcOffset);
  EEPROM.put(LAT_ADDR, latitude);
  EEPROM.put(LON_ADDR, longitude);
  EEPROM.write(DST_MODE_ADDR, dstMode);
  EEPROM.write(DST_ACTIVE_ADDR, dstActiveManual);
  EEPROM.commit();
}

void saveWiFiToEEPROM(const String& ssid, const String& pass) {
  for (int i = 0; i < 32; i++) {
    EEPROM.write(SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
    EEPROM.write(PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  }
  EEPROM.commit();
}

void loadSettingsFromEEPROM() {
  char ssid[33], pass[33];
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(SSID_ADDR + i);
    pass[i] = EEPROM.read(PASS_ADDR + i);
  }
  ssid[32] = pass[32] = '\0';
  storedSSID = String(ssid);
  storedPASS = String(pass);
  EEPROM.get(SUNRISE_OFFSET_ADDR, sunriseOffsetMin);
  EEPROM.get(SUNSET_OFFSET_ADDR, sunsetOffsetMin);
  EEPROM.get(TZ_OFFSET_ADDR, utcOffset);
  EEPROM.get(LAT_ADDR, latitude);
  EEPROM.get(LON_ADDR, longitude);
  dstMode = EEPROM.read(DST_MODE_ADDR);
  dstActiveManual = EEPROM.read(DST_ACTIVE_ADDR);
  int dstAdjust = (dstMode == 1 && dstActiveManual == 1) ? 3600 : 0;
  timeClient.setTimeOffset(utcOffset + dstAdjust);
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
  time_t now = timeClient.getEpochTime();
  sunriseRaw = getSunEventUTC(now, true, latitude, longitude) + timeClient.getTimeOffset();
  sunsetRaw  = getSunEventUTC(now, false, latitude, longitude) + timeClient.getTimeOffset();
  sunriseFinal = sunriseRaw + sunriseOffsetMin * 60;
  sunsetFinal  = sunsetRaw  + sunsetOffsetMin * 60;
}

String formatTime(time_t t) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour(t), minute(t), second(t));
  return String(buf);
}

void handleRoot() {
  time_t now = timeClient.getEpochTime();
  String page = "<h1>Astro Light Control</h1>";
  page += "<p>Current time: " + formatTime(now) + "</p>";
  page += "<p>Time zone offset: UTC" + String(utcOffset / 3600) + "</p>";
  page += "<p>Latitude: " + String(latitude, 6) + ", Longitude: " + String(longitude, 6) + "</p>";
  page += "<p>Sunrise: " + formatTime(sunriseFinal) + " (raw: " + formatTime(sunriseRaw) + ")</p>";
  page += "<p>Sunset: " + formatTime(sunsetFinal) + " (raw: " + formatTime(sunsetRaw) + ")</p>";
  page += R"rawliteral(
    <hr><form method='POST' action='/save'>
      SSID:<br><input name='ssid'><br>
      Password:<br><input name='pass' type='password'><br>
      Timezone offset (hours):<br><input name='tz'><br>
      Sunrise offset (min):<br><input name='sunrise'><br>
      Sunset offset (min):<br><input name='sunset'><br>
      Latitude:<br><input name='lat'><br>
      Longitude:<br><input name='lon'><br>
      DST Mode:<br>
      <select name='dstmode' onchange='document.getElementById("dstactive").style.display = (this.value=="1") ? "block" : "none";'>
        <option value='0'>Off</option>
        <option value='1'>Manual</option>
        <option value='2'>Auto</option>
      </select><br>
      <div id='dstactive' style='display:none'>
        DST Active (0=Standard, 1=Summer):<br><input name='dstactive'><br>
      </div><br>
      <input type='submit' value='Save & Reboot'>
    </form>
    <script>
      document.querySelector("[name=dstmode]").value = ")rawliteral" + String(dstMode) + R"rawliteral(";
      if (")rawliteral" + String(dstMode) + R"rawliteral(" == "1") document.getElementById("dstactive").style.display = "block";
    </script>
  )rawliteral";
  server.send(200, "text/html", page);
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(storedSSID.c_str(), storedPASS.c_str());
  Serial.print("Connecting to Wi-Fi");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected: " + WiFi.localIP().toString());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("AstroSetup");
    Serial.println("\nAP Mode. IP: " + WiFi.softAPIP().toString());
  }
}

void startWebInterface() {
  server.on("/", handleRoot);
  server.on("/save", []() {
    if (server.hasArg("ssid") && server.arg("ssid") != "") storedSSID = server.arg("ssid");
    if (server.hasArg("pass") && server.arg("pass") != "") storedPASS = server.arg("pass");
    if (server.hasArg("tz") && server.arg("tz") != "") utcOffset = server.arg("tz").toInt() * 3600;
    if (server.hasArg("sunrise")) sunriseOffsetMin = server.arg("sunrise").toInt();
    if (server.hasArg("sunset")) sunsetOffsetMin = server.arg("sunset").toInt();
    if (server.hasArg("lat")) latitude = server.arg("lat").toFloat();
    if (server.hasArg("lon")) longitude = server.arg("lon").toFloat();
    if (server.hasArg("dstmode")) dstMode = server.arg("dstmode").toInt();
    if (server.hasArg("dstactive")) dstActiveManual = server.arg("dstactive").toInt();

    int dstAdjust = (dstMode == 1 && dstActiveManual == 1) ? 3600 : 0;
    timeClient.setTimeOffset(utcOffset + dstAdjust);
    saveWiFiToEEPROM(storedSSID, storedPASS);
    saveSettingsToEEPROM();
    server.send(200, "text/html", "<h1>Saved. Rebooting...</h1>");
    delay(1500);
    ESP.restart();
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  pinMode(5, OUTPUT);
  digitalWrite(5, LOW);
  EEPROM.begin(EEPROM_SIZE);
  loadSettingsFromEEPROM();
  setupWiFi();
  timeClient.begin();
  if (timeClient.forceUpdate()) lastSyncTime = timeClient.getEpochTime();
  updateSunTimes();
  startWebInterface();
}

void loop() {
  server.handleClient();
}
