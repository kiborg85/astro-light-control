#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>

ESP8266WebServer server(80);

// ==== НАСТРОЙКИ ПО УМОЛЧАНИЮ ====
const char* ssid = "your_wifi_ssid";     // Заменить на свой SSID
const char* password = "your_wifi_pass"; // Заменить на свой пароль

const float latitude = 46.4825;   // Одесса, для примера
const float longitude = 30.7233;
const int utcOffset = 3 * 3600;   // +03:00 в секундах

const int sunsetOffset = 0;       // в секундах
const int sunriseOffset = 0;

const int relayPin = 5;           // GPIO5 = D1 на NodeMCU

// ==== ВРЕМЯ ====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffset, 3600 * 1000);  // обновление каждый час

time_t sunriseTime = 0;
time_t sunsetTime = 0;

// ==== ПОДКЛЮЧЕНИЕ К WI-FI ====
void connectWiFiOrStartAP() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to Wi-Fi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWi-Fi failed. Starting Access Point...");

    WiFi.mode(WIFI_AP);
    WiFi.softAP("SunlightSetup");

    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
  }
  if (WiFi.status() == WL_CONNECTED) {
  Serial.println("\nWi-Fi connected. IP: " + WiFi.localIP().toString());
} else {
  Serial.println("\nWi-Fi failed. Starting Access Point...");

  WiFi.mode(WIFI_AP);
  WiFi.softAP("SunlightSetup");

  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(apIP);

  // Веб-страница по адресу http://192.168.4.1/
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

      Serial.println("Saving Wi-Fi credentials:");
      Serial.println("SSID: " + ssid);
      Serial.println("PASS: " + pass);

      // Сохраняем в EEPROM (пока не реализовано)
      // позже добавим функцию saveWiFiToEEPROM(ssid, pass);

      server.send(200, "text/html", "<h1>Saved. Rebooting...</h1>");
      delay(2000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Bad Request");
    }
  });


  server.begin();
  Serial.println("HTTP server started");
}

}

// ==== ПРОСТОЙ РАСЧЁТ ВРЕМЕНИ СОЛНЦА ====
time_t calculateSunEvent(bool isSunrise) {
  // В будущем — заменить на реальный астрономический расчёт
  time_t now = timeClient.getEpochTime();
  tmElements_t tm;
  breakTime(now, tm);
  tm.Hour = isSunrise ? 5 : 21; // Условно: восход в 5:00, закат в 21:00
  tm.Minute = 0;
  tm.Second = 0;
  return makeTime(tm);
}

void updateSunTimes() {
  sunriseTime = calculateSunEvent(true) + sunriseOffset;
  sunsetTime = calculateSunEvent(false) + sunsetOffset;

  Serial.print("Sunrise time: ");
  Serial.print(hour(sunriseTime));
  Serial.print(":");
  Serial.println(minute(sunriseTime));

  Serial.print("Sunset time: ");
  Serial.print(hour(sunsetTime));
  Serial.print(":");
  Serial.println(minute(sunsetTime));
}

void controlRelay(time_t now) {
  if (now >= sunsetTime || now <= sunriseTime) {
    digitalWrite(relayPin, HIGH); // ВКЛ свет
    Serial.println("Relay ON");
  } else {
    digitalWrite(relayPin, LOW);  // ВЫКЛ
    Serial.println("Relay OFF");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP8266 Sunlight Switch ===");

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  connectWiFiOrStartAP();

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.begin();
    if (timeClient.forceUpdate()) {
      Serial.println("Time synced: " + timeClient.getFormattedTime());
      updateSunTimes();
    } else {
      Serial.println("Time sync failed.");
    }
  }
}

void loop() {
  if (WiFi.getMode() == WIFI_AP) {
      server.handleClient();
  }

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    time_t now = timeClient.getEpochTime();

    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 60000) {  // каждую минуту
      Serial.print("Current time: ");
      Serial.print(hour(now));
      Serial.print(":");
      Serial.println(minute(now));

      controlRelay(now);
      lastUpdate = millis();
    }
  } else {
    // без Wi-Fi просто не трогаем реле
    delay(5000);
  }
}
