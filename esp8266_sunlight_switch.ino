#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <EEPROM.h>

// ===================== НАСТРОЙКИ ПО УМОЛЧАНИЮ =====================
const char* ssid = "your_wifi_ssid";
const char* password = "your_wifi_password";

// Местоположение (пример: Одесса)
float latitude = 46.4825;
float longitude = 30.7233;
int utcOffset = 3 * 3600; // смещение по UTC в секундах

// Смещения для реле (в секундах)
int sunsetOffset = 0;
int sunriseOffset = 0;

// Пин реле
const int relayPin = D1;

// ===================== ОБЪЕКТЫ =====================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffset, 3600 * 1000);  // обновление времени раз в час

// ===================== ВРЕМЕННЫЕ ПЕРЕМЕННЫЕ =====================
time_t sunriseTime = 0;
time_t sunsetTime = 0;

// ===================== ФУНКЦИИ =====================

void connectWiFi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.print(ssid);
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP address: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi connection failed.");
  }
}

// Упрощённый расчёт восхода/заката
time_t calculateSunEvent(bool isSunrise) {
  // пример — статичное время (в будущем заменим на точные астрономические расчёты)
  time_t now = timeClient.getEpochTime();
  tmElements_t tm;
  breakTime(now, tm);
  tm.Hour = isSunrise ? 5 : 21;
  tm.Minute = 0;
  tm.Second = 0;
  return makeTime(tm);
}

void updateSunTimes() {
  sunriseTime = calculateSunEvent(true) + sunriseOffset;
  sunsetTime = calculateSunEvent(false) + sunsetOffset;
  Serial.println("Sunrise: " + String(hour(sunriseTime)) + ":" + String(minute(sunriseTime)));
  Serial.println("Sunset: " + String(hour(sunsetTime)) + ":" + String(minute(sunsetTime)));
}

void controlRelay(time_t now) {
  if (now >= sunsetTime || now <= sunriseTime) {
    digitalWrite(relayPin, HIGH); // включить свет
    Serial.println("Relay ON");
  } else {
    digitalWrite(relayPin, LOW); // выключить
    Serial.println("Relay OFF");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP8266 Sunlight Switch ===");

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  EEPROM.begin(512); // если будут настройки

  connectWiFi();
  timeClient.begin();

  if (timeClient.forceUpdate()) {
    Serial.println("Time synced: " + timeClient.getFormattedTime());
    updateSunTimes();
  } else {
    Serial.println("Failed to sync time.");
  }
}

void loop() {
  timeClient.update();
  time_t now = timeClient.getEpochTime();

  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 60 * 1000) {  // проверка каждую минуту
    Serial.println("Current time: " + String(hour(now)) + ":" + String(minute(now)));
    controlRelay(now);
    lastUpdate = millis();
  }
}
