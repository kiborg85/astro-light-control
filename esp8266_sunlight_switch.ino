#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <EEPROM.h>

// ==== Параметры ====
#define EEPROM_SIZE 200
#define SSID_ADDR 0
#define PASS_ADDR 100

const int relayPin = 5; // GPIO5 = D1 на NodeMCU
const int utcOffset = 3 * 3600; // +03:00 в секундах

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffset, 3600 * 1000);
ESP8266WebServer server(80);

String storedSSID;
String storedPASS;

time_t sunriseTime = 0;
time_t sunsetTime = 0;

// ==== EEPROM ====
void saveWiFiToEEPROM(const String& ssid, const String& pass) {
  Serial.println("Saving Wi-Fi to EEPROM...");
  for (int i = 0; i < 32; i++) {
    EEPROM.write(SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
    EEPROM.write(PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  }
  EEPROM.commit();
  Serial.println("Saved.");
}

void loadWiFiFromEEPROM() {
  char ssid[33];
  char pass[33];
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(SSID_ADDR + i);
    pass[i] = EEPROM.read(PASS_ADDR + i);
  }
  ssid[32] = '\0';
  pass[32] = '\0';
  storedSSID = String(ssid);
  storedPASS = String(pass);
  Serial.println("Loaded from EEPROM:");
  Serial.println("SSID: " + storedSSID);
  Serial.println("PASS: " + storedPASS);
}

// ==== Wi-Fi ====
void connectWiFiFromEEPROM() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(storedSSID.c_str(), storedPASS.c_str());
  Serial.print("Connecting to Wi-Fi: ");
  Serial.print(storedSSID);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWi-Fi connection failed.");
  }
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("SunlightSetup");
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(apIP);

  server.on("/", []() {
    String html = R"rawliteral(
      <h1>Wi-Fi Setup</h1>
      <form method='POST' action='/save'>
        SSID:<br><input type='text' name='ssid'><br>
        Password:<br><input type='password' name='pass'><br><br>
        <input type='submit' value='Save & Reboot'>
      </form>
    )rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/save", []() {
    if (server.hasArg("ssid") && server.hasArg("pass")) {
      String ssid = server.arg("ssid");
      String pass = server.arg("pass");

      Serial.println("Received new Wi-Fi credentials:");
      Serial.println("SSID: " + ssid);
      Serial.println("PASS: " + pass);

      saveWiFiToEEPROM(ssid, pass);
      server.send(200, "text/html", "<h1>Saved. Rebooting...</h1>");
      delay(2000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Bad Request");
    }
  });

  server.begin();
  Serial.println("HTTP server started.");
}

// ==== Восход/Закат (заглушка) ====
time_t calculateSunEvent(bool isSunrise) {
  time_t now = timeClient.getEpochTime();
  tmElements_t tm;
  breakTime(now, tm);
  tm.Hour = isSunrise ? 5 : 21;
  tm.Minute = 0;
  tm.Second = 0;
  return makeTime(tm);
}

void updateSunTimes() {
  sunriseTime = calculateSunEvent(true);
  sunsetTime = calculateSunEvent(false);
  Serial.print("Sunrise: ");
  Serial.println(String(hour(sunriseTime)) + ":" + String(minute(sunriseTime)));
  Serial.print("Sunset: ");
  Serial.println(String(hour(sunsetTime)) + ":" + String(minute(sunsetTime)));
}

void controlRelay(time_t now) {
  if (now >= sunsetTime || now <= sunriseTime) {
    digitalWrite(relayPin, HIGH);
    Serial.println("Relay ON");
  } else {
    digitalWrite(relayPin, LOW);
    Serial.println("Relay OFF");
  }
}

// ==== SETUP ====
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP8266 Sunlight Switch ===");

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  EEPROM.begin(EEPROM_SIZE);
  loadWiFiFromEEPROM();
  connectWiFiFromEEPROM();

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.begin();
    if (timeClient.forceUpdate()) {
      Serial.println("Time synced.");
      updateSunTimes();
    } else {
      Serial.println("NTP sync failed.");
    }
  } else {
    startAccessPoint();
  }
}

// ==== LOOP ====
void loop() {
  if (WiFi.getMode() == WIFI_AP) {
    server.handleClient();
    delay(10);
    return;
  }

  timeClient.update();
  time_t now = timeClient.getEpochTime();

  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 60000) {
    Serial.print("Current time: ");
    Serial.println(String(hour(now)) + ":" + String(minute(now)));
    controlRelay(now);
    lastUpdate = millis();
  }
}
