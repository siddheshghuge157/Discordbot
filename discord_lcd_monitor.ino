#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "ThingSpeak.h"
#include "DHT.h"
#include "time.h"
#include <ArduinoJson.h>
#include <LiquidCrystal_SR.h>
#include "secrets.h"

WiFiClient thingSpeakClient;

WiFiClientSecure discordClient;
String lastMessageId = "";

LiquidCrystal_SR lcd(17, 16, 21);

enum LcdMode { LCD_TIME, LCD_SENSORS_SCROLL, LCD_FLASH_ALERT, LCD_STOPWATCH, LCD_TIMER };
LcdMode currentLcdMode = LCD_TIME;

String scrollText = "";
int scrollIndex = 0;
unsigned long lastScrollTime = 0;
const unsigned long scrollInterval = 250;

bool flashVisible = true;
unsigned long lastFlashTime = 0;
const unsigned long flashInterval = 500;

bool stopwatchRunning = false;
unsigned long stopwatchStart = 0;
unsigned long stopwatchElapsed = 0;

bool timerActive = false;
unsigned long timerEndMillis = 0;
unsigned long timerDuration = 0;
bool timerAlarmOn = false;

unsigned long lastLcdUpdate = 0;
const unsigned long lcdUpdateInterval = 100;

const int stopwatchButtonPin = 0;
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool buttonPressed = false;
unsigned long buttonPressStart = 0;
const unsigned long longPressTime = 800;
unsigned long buttonIgnoreUntil = 0;

float tempThreshold = 35.0;
float distThreshold = 10.0;
bool alertsEnabled = true;

const int potPin = 34;
const int ldrPin = 35;
#define DHTPIN 19
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
const int trigPin = 25;
const int echoPin = 26;

const int ledPin = 13;
const int buzzerPin = 15;

float potValue = 0;
float ldrValue = 0;
float temperatureC = 0;
float humidity = 0;
float distanceCm = 0;

unsigned long lastThingSpeakUpdate = 0;
const unsigned long thingSpeakInterval = 30000;
unsigned long lastDiscordCheck = 0;
const unsigned long discordCheckInterval = 3000;

void sendDiscordMessage(String content) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!discordClient.connect("discord.com", 443)) return;

  StaticJsonDocument<256> doc;
  doc["content"] = content;
  String json;
  serializeJson(doc, json);

  String url = "/api/v10/channels/" + String(CHANNEL_ID) + "/messages";
  discordClient.println("POST " + url + " HTTP/1.0");
  discordClient.println("Host: discord.com");
  discordClient.println("Authorization: Bot " + String(DISCORD_BOT_TOKEN));
  discordClient.println("Content-Type: application/json");
  discordClient.print("Content-Length: ");
  discordClient.println(json.length());
  discordClient.println();
  discordClient.println(json);

  while (discordClient.connected() && !discordClient.available()) delay(10);
  while (discordClient.available()) discordClient.readStringUntil('\n');
  discordClient.stop();
}

String getLatestDiscordMessage() {
  if (WiFi.status() != WL_CONNECTED) return "";
  if (!discordClient.connect("discord.com", 443)) return "";

  String url = "/api/v10/channels/" + String(CHANNEL_ID) + "/messages?limit=1";
  discordClient.println("GET " + url + " HTTP/1.0");
  discordClient.println("Host: discord.com");
  discordClient.println("Authorization: Bot " + String(DISCORD_BOT_TOKEN));
  discordClient.println("Content-Type: application/json");
  discordClient.println();

  while (discordClient.connected() && !discordClient.available()) delay(10);
  while (discordClient.available()) {
    String line = discordClient.readStringUntil('\r');
    if (line == "\n") break;
  }

  String json = "";
  while (discordClient.available()) json += discordClient.readString();
  discordClient.stop();

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) return "";

  JsonArray messages = doc.as<JsonArray>();
  if (messages.size() == 0) return "";

  JsonObject message = messages[0];
  String author = message["author"]["username"];
  String content = message["content"];
  String msgId = message["id"];

  if (author == "ESP32") return "";
  if (msgId != lastMessageId) {
    lastMessageId = msgId;
    return content;
  }
  return "";
}

String getThingSpeakData(int fieldNum, int results) {
  if (WiFi.status() != WL_CONNECTED) return "❌ WiFi not connected.";
  if (fieldNum < 1 || fieldNum > 8) return "❌ Field must be between 1 and 8.";
  if (results < 1 || results > 8000) return "❌ Results must be between 1 and 8000.";

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect("api.thingspeak.com", 443)) {
    Serial.println("TS: connection failed");
    return "❌ Could not connect to ThingSpeak.";
  }

  String url = "/channels/" + String(myChannelNumber) + "/fields/" + String(fieldNum) + ".json";
  url += "?api_key=" + String(myReadAPIKey);
  url += "&results=" + String(results);

  client.println("GET " + url + " HTTP/1.0");
  client.println("Host: api.thingspeak.com");
  client.println("Connection: close");
  client.println();

  String response = "";
  unsigned long timeout = millis();
  while (client.connected() || client.available()) {
    if (client.available()) {
      response += client.readString();
      timeout = millis();
    }
    if (millis() - timeout > 5000) {
      Serial.println("TS: timeout waiting for data");
      break;
    }
  }
  client.stop();

  Serial.println("=== FULL TS RESPONSE ===");
  Serial.println(response);
  Serial.println("=== END TS RESPONSE ===");
  Serial.flush();

  int jsonStart = response.indexOf("\r\n\r\n");
  if (jsonStart == -1) jsonStart = response.indexOf("\n\n");
  if (jsonStart == -1) {
    Serial.println("ERROR: Could not find end of HTTP headers.");
    return "❌ Invalid response from ThingSpeak (no header end).";
  }
  jsonStart += (response.charAt(jsonStart+1) == '\n') ? 2 : 4;
  String json = response.substring(jsonStart);
  json.trim();

  Serial.println("=== EXTRACTED JSON ===");
  Serial.println(json);
  Serial.flush();

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.print("TS JSON parse error: ");
    Serial.println(error.c_str());
    return "❌ Could not parse ThingSpeak data.";
  }

  JsonObject root = doc.as<JsonObject>();
  JsonArray feeds = root["feeds"];
  if (!feeds || feeds.size() == 0) {
    Serial.println("TS: feeds array empty or missing.");
    return "❌ No data found for field " + String(fieldNum) + ".";
  }

  String reply = "📈 **Field " + String(fieldNum) + " – Last " + String(feeds.size()) + " readings:**\n";
  for (JsonObject feed : feeds) {
    String timeStr = feed["created_at"];
    timeStr = timeStr.substring(0, 19);
    timeStr.replace("T", " ");
    String value = feed["field" + String(fieldNum)];
    reply += "`" + timeStr + "` → " + value + "\n";
  }

  if (reply.length() > 2000) reply = reply.substring(0, 1950) + "\n... (truncated)";
  return reply;
}
void updateLcd() {
  lcd.clear();
  switch (currentLcdMode) {
    case LCD_TIME: {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        char timeStr[20];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
        lcd.setCursor(0, 0);
        lcd.print("Time: ");
        lcd.print(timeStr);

        char dateStr[20];
        strftime(dateStr, sizeof(dateStr), "%d/%m/%Y", &timeinfo);
        lcd.setCursor(0, 1);
        lcd.print("Date: ");
        lcd.print(dateStr);
      } else {
        lcd.setCursor(0, 0);
        lcd.print("Time not set");
      }
      break;
    }

    case LCD_SENSORS_SCROLL: {
      scrollText = "Pot:" + String(potValue, 1) +
                   " LDR:" + String(ldrValue, 1) +
                   " Temp:" + String(temperatureC, 1) + "C" +
                   " Hum:" + String(humidity, 1) + "%" +
                   " Dist:" + String(distanceCm, 1) + "cm   ";

      if (millis() - lastScrollTime > scrollInterval) {
        lastScrollTime = millis();
        scrollIndex++;
        if (scrollIndex >= scrollText.length()) scrollIndex = 0;
      }

      lcd.setCursor(0, 0);
      lcd.print(scrollText.substring(scrollIndex));
      lcd.setCursor(0, 1);
      lcd.print("Sensors: scroll");
      break;
    }

    case LCD_FLASH_ALERT: {
      if (millis() - lastFlashTime > flashInterval) {
        lastFlashTime = millis();
        flashVisible = !flashVisible;
      }
      if (flashVisible) {
        lcd.setCursor(0, 0);
        lcd.print("⚠️ ALERT ⚠️");
        lcd.setCursor(0, 1);
        lcd.print("Check sensors!");
      }
      break;
    }

    case LCD_STOPWATCH: {
      unsigned long elapsed = stopwatchElapsed;
      if (stopwatchRunning) elapsed = stopwatchElapsed + (millis() - stopwatchStart);
      int mins = elapsed / 60000;
      int secs = (elapsed / 1000) % 60;
      int tenths = (elapsed / 100) % 10;

      char buf[17];
      sprintf(buf, "Stopw: %02d:%02d.%01d", mins, secs, tenths);
      lcd.setCursor(0, 0);
      lcd.print(buf);
      lcd.setCursor(0, 1);
      lcd.print(stopwatchRunning ? "> RUNNING     " : "> STOPPED     ");
      break;
    }

    case LCD_TIMER: {
      if (timerActive) {
        long remaining = (timerEndMillis - millis()) / 1000;
        if (remaining < 0) remaining = 0;
        int mins = remaining / 60;
        int secs = remaining % 60;
        char buf[17];
        sprintf(buf, "Timer: %02d:%02d  ", mins, secs);
        lcd.setCursor(0, 0);
        lcd.print(buf);
        lcd.setCursor(0, 1);
        lcd.print("Counting down...");
      } else if (timerAlarmOn) {
        if (millis() - lastFlashTime > flashInterval) {
          lastFlashTime = millis();
          flashVisible = !flashVisible;
        }
        if (flashVisible) {
          lcd.setCursor(0, 0);
          lcd.print("⏰ TIME'S UP ⏰");
          lcd.setCursor(0, 1);
          lcd.print("!timeroff to stop");
        }
      } else {
        lcd.setCursor(0, 0);
        lcd.print("No timer set");
        lcd.setCursor(0, 1);
        lcd.print("Use !timer <sec>");
      }
      break;
    }
  }
}

void handleStopwatchButton() {
  if (millis() < buttonIgnoreUntil) return;

  bool reading = digitalRead(stopwatchButtonPin);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonPressed) {
      buttonPressed = reading;

      if (buttonPressed == LOW) {
        buttonPressStart = millis();
      } else {
        unsigned long pressDuration = millis() - buttonPressStart;
        if (pressDuration < longPressTime) {
          if (currentLcdMode != LCD_STOPWATCH) {
            currentLcdMode = LCD_STOPWATCH;
            sendDiscordMessage("⏱️ LCD switched to Stopwatch mode.");
          }
          if (stopwatchRunning) {
            stopwatchElapsed += millis() - stopwatchStart;
            stopwatchRunning = false;
            sendDiscordMessage("⏸️ Stopwatch stopped.");
          } else {
            stopwatchStart = millis();
            stopwatchRunning = true;
            sendDiscordMessage("▶️ Stopwatch started.");
          }
        } else {
          if (currentLcdMode != LCD_STOPWATCH) {
            currentLcdMode = LCD_STOPWATCH;
          }
          stopwatchRunning = false;
          stopwatchElapsed = 0;
          stopwatchStart = 0;
          sendDiscordMessage("🔄 Stopwatch reset.");
        }
      }
    }
  }
  lastButtonState = reading;
}

void handleCommand(String cmd) {
  cmd.trim();
  Serial.println("Command received: " + cmd);

  if (cmd == "!status") {
    String reply = "📊 **Current Status**\n";
    reply += "Pot: " + String(potValue, 1) + "\n";
    reply += "LDR: " + String(ldrValue, 1) + "\n";
    reply += "Temp: " + String(temperatureC, 1) + " °C\n";
    reply += "Humidity: " + String(humidity, 1) + " %\n";
    reply += "Distance: " + String(distanceCm, 1) + " cm\n";
    reply += "Temp Threshold: " + String(tempThreshold, 1) + " °C\n";
    reply += "Dist Threshold: " + String(distThreshold, 1) + " cm\n";
    reply += "LED: " + String(digitalRead(ledPin) ? "ON" : "OFF") + "\n";
    reply += "Buzzer: " + String(digitalRead(buzzerPin) ? "ON" : "OFF");
    sendDiscordMessage(reply);
  }
  else if (cmd.startsWith("!temp ")) {
    float val = cmd.substring(6).toFloat();
    if (val > 0) {
      tempThreshold = val;
      sendDiscordMessage("✅ Temperature threshold set to " + String(tempThreshold, 1) + " °C");
    } else {
      sendDiscordMessage("❌ Usage: !temp 38");
    }
  }
  else if (cmd.startsWith("!dist ")) {
    float val = cmd.substring(6).toFloat();
    if (val > 0) {
      distThreshold = val;
      sendDiscordMessage("✅ Distance threshold set to " + String(distThreshold, 1) + " cm");
    } else {
      sendDiscordMessage("❌ Usage: !dist 15");
    }
  }
  else if (cmd == "!ledon") {
    digitalWrite(ledPin, HIGH);
    sendDiscordMessage("💡 LED turned ON");
  }
  else if (cmd == "!ledoff") {
    digitalWrite(ledPin, LOW);
    sendDiscordMessage("💡 LED turned OFF");
  }
  else if (cmd == "!buzzon") {
    digitalWrite(buzzerPin, HIGH);
    sendDiscordMessage("🔔 Buzzer turned ON");
  }
  else if (cmd == "!buzzoff") {
    digitalWrite(buzzerPin, LOW);
    sendDiscordMessage("🔔 Buzzer turned OFF");
  }
  else if (cmd == "!history") {
    sendDiscordMessage("⏳ Fetching last 5 temperature readings...");
    String result = getThingSpeakData(3, 5);
    sendDiscordMessage(result);
  }
  else if (cmd.startsWith("!read ")) {
    String params = cmd.substring(6);
    int spaceIdx = params.indexOf(' ');
    if (spaceIdx == -1) {
      sendDiscordMessage("❌ Usage: !read <field> <count>\nExample: !read 3 10");
      return;
    }
    int fieldNum = params.substring(0, spaceIdx).toInt();
    int count = params.substring(spaceIdx + 1).toInt();
    sendDiscordMessage("⏳ Fetching data for field " + String(fieldNum) + "...");
    String result = getThingSpeakData(fieldNum, count);
    sendDiscordMessage(result);
  }
  else if (cmd == "!lcd time") {
    currentLcdMode = LCD_TIME;
    sendDiscordMessage("🕒 LCD set to Time mode.");
  }
  else if (cmd == "!lcd sensors") {
    currentLcdMode = LCD_SENSORS_SCROLL;
    scrollIndex = 0;
    sendDiscordMessage("📊 LCD set to Scrolling Sensors mode.");
  }
  else if (cmd == "!lcd flash") {
    currentLcdMode = LCD_FLASH_ALERT;
    flashVisible = true;
    sendDiscordMessage("⚠️ LCD set to Flashing Alert mode.");
  }
  else if (cmd == "!lcd stopwatch") {
    currentLcdMode = LCD_STOPWATCH;
    sendDiscordMessage("⏱️ LCD set to Stopwatch mode. Use !sw start, !sw stop, !sw reset.");
  }
  else if (cmd == "!lcd timer") {
    currentLcdMode = LCD_TIMER;
    sendDiscordMessage("⏲️ LCD set to Timer mode.");
  }
  else if (cmd == "!sw start") {
    if (currentLcdMode != LCD_STOPWATCH) {
      sendDiscordMessage("First switch to stopwatch mode: !lcd stopwatch");
      return;
    }
    if (!stopwatchRunning) {
      stopwatchStart = millis();
      stopwatchRunning = true;
      sendDiscordMessage("▶️ Stopwatch started.");
    }
  }
  else if (cmd == "!sw stop") {
    if (currentLcdMode != LCD_STOPWATCH) {
      sendDiscordMessage("First switch to stopwatch mode: !lcd stopwatch");
      return;
    }
    if (stopwatchRunning) {
      stopwatchElapsed += millis() - stopwatchStart;
      stopwatchRunning = false;
      sendDiscordMessage("⏸️ Stopwatch stopped.");
    }
  }
  else if (cmd == "!sw reset") {
    stopwatchRunning = false;
    stopwatchElapsed = 0;
    stopwatchStart = 0;
    sendDiscordMessage("🔄 Stopwatch reset.");
  }
  else if (cmd.startsWith("!timer ")) {
    int sec = cmd.substring(7).toInt();
    if (sec > 0 && sec <= 86400) {
      timerDuration = sec;
      timerEndMillis = millis() + sec * 1000UL;
      timerActive = true;
      timerAlarmOn = false;
      digitalWrite(buzzerPin, LOW);
      digitalWrite(ledPin, LOW);
      sendDiscordMessage("⏲️ Timer set for " + String(sec) + " seconds. Use `!lcd timer` to view countdown.");
    } else {
      sendDiscordMessage("❌ Invalid time. Use: !timer 30");
    }
  }
  else if (cmd == "!timeroff") {
    timerActive = false;
    timerAlarmOn = false;
    digitalWrite(buzzerPin, LOW);
    digitalWrite(ledPin, LOW);
    sendDiscordMessage("🔕 Timer alarm silenced and timer cleared.");
  }
  else {
    sendDiscordMessage("❓ Unknown command. Try:\n"
                       "`!status`, `!temp 40`, `!dist 10`, `!ledon`, `!buzzon`,\n"
                       "`!history`, `!read 3 10`, `!lcd time/sensors/flash/stopwatch/timer`,\n"
                       "`!sw start/stop/reset`, `!timer <seconds>`, `!timeroff`");
  }
}

void checkTimer() {
  if (timerActive && millis() >= timerEndMillis) {
    timerActive = false;
    timerAlarmOn = true;
    digitalWrite(buzzerPin, HIGH);
    digitalWrite(ledPin, HIGH);
    sendDiscordMessage("⏰ **TIME'S UP!** Timer of " + String(timerDuration) + " seconds finished. Use `!timeroff` to silence.");
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  ThingSpeak.begin(thingSpeakClient);
  dht.begin();

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  digitalWrite(buzzerPin, LOW);
  pinMode(stopwatchButtonPin, INPUT_PULLUP);

  lcd.begin(16, 2);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Starting...");

  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
  unsigned long syncStart = millis();
  time_t now = time(NULL);
  while (now < 100000 && (millis() - syncStart) < 15000) {
    delay(300);
    Serial.print(".");
    now = time(NULL);
  }
  if (now < 100000) {
    Serial.println("\nTime sync failed! Using internal clock.");
  } else {
    Serial.println("\nTime synced.");
  }

  discordClient.setInsecure();

  buttonIgnoreUntil = millis() + 5000;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ESP32 Ready");
  delay(1000);

  sendDiscordMessage("🚀 ESP32 Monitor bot is now online! Type `!status` to begin.");
  Serial.println("Startup message sent.");
}

void loop() {
  if (millis() - lastDiscordCheck > discordCheckInterval) {
    lastDiscordCheck = millis();
    String cmd = getLatestDiscordMessage();
    if (cmd.length() > 0) handleCommand(cmd);
  }

  handleStopwatchButton();
  checkTimer();

  if (millis() - lastLcdUpdate > lcdUpdateInterval) {
    lastLcdUpdate = millis();
    updateLcd();
  }

  if (millis() - lastThingSpeakUpdate > thingSpeakInterval) {
    lastThingSpeakUpdate = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("Reconnecting WiFi...");
      WiFi.begin(ssid, password);
      int retry = 0;
      while (WiFi.status() != WL_CONNECTED && retry++ < 10) delay(1000);
      if (WiFi.status() == WL_CONNECTED) Serial.println("Done.");
    }

    potValue = analogRead(potPin);
    ldrValue = analogRead(ldrPin);
    temperatureC = dht.readTemperature();
    humidity = dht.readHumidity();
    if (isnan(temperatureC) || isnan(humidity)) { temperatureC = 0; humidity = 0; }
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long duration = pulseIn(echoPin, HIGH);
    distanceCm = duration * 0.0343 / 2;

    Serial.printf("Pot:%.1f LDR:%.1f Temp:%.1f Hum:%.1f Dist:%.1f cm\n",
                  potValue, ldrValue, temperatureC, humidity, distanceCm);

    if (alertsEnabled) {
      static bool tempAlertSent = false;
      if (temperatureC > tempThreshold && !tempAlertSent) {
        sendDiscordMessage("⚠️ ALERT! High Temperature: " + String(temperatureC, 1) + " °C");
        tempAlertSent = true;
      } else if (temperatureC <= tempThreshold) tempAlertSent = false;

      static bool distAlertSent = false;
      if (distanceCm < distThreshold && distanceCm > 1.0 && !distAlertSent) {
        sendDiscordMessage("🚨 ALERT! Object too close: " + String(distanceCm, 1) + " cm");
        distAlertSent = true;
      } else if (distanceCm >= distThreshold) distAlertSent = false;
    }

    ThingSpeak.setField(1, potValue);
    ThingSpeak.setField(2, ldrValue);
    ThingSpeak.setField(3, temperatureC);
    ThingSpeak.setField(4, humidity);
    ThingSpeak.setField(5, distanceCm);
    int x = ThingSpeak.writeFields(myChannelNumber, myWriteAPIKey);
    if (x == 200) Serial.println("ThingSpeak OK");
    else Serial.println("ThingSpeak error: " + String(x));
  }
}
