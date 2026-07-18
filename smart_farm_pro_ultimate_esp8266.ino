#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <DHT.h>
#include <RTClib.h>
#include <Wire.h>
#include <Ticker.h>
#include <UniversalTelegramBot.h>
#include <EEPROM.h>

// ==================================================================================================================
// 1. WiFi Configuration
// ==================================================================================================================
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ==================================================================================================================
// 2. MQTT Configuration (HiveMQ Cloud with TLS)
// ==================================================================================================================
const char* mqtt_server = "650188a0ee2b4367b7c131fb385590a9.s1.eu.hivemq.cloud";
const int mqtt_port = 8883; // TLS port
const char* mqtt_username = "smartfarm";
const char* mqtt_password = "Kla12345";
String mqtt_client_id = "ESP8266_" + String(ESP.getChipId(), HEX); // Unique Client ID

// MQTT Topics
const char* MQTT_TOPIC_SENSOR_DATA = "smartfarm/sensor/data";
const char* MQTT_TOPIC_PUMP_STATUS = "smartfarm/pump/status";
const char* MQTT_TOPIC_ZONE_STATUS = "smartfarm/zone/%d/status"; // %d for zone number
const char* MQTT_TOPIC_AUTO_STATUS = "smartfarm/auto/status";
const char* MQTT_TOPIC_COMMAND = "smartfarm/cmd/#"; // Subscribe to all commands
const char* MQTT_TOPIC_LOG = "smartfarm/log";

// ==================================================================================================================
// 3. Telegram Configuration
// ==================================================================================================================
#define TELEGRAM_BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN" // Replace with your bot token
#define TELEGRAM_CHAT_ID "YOUR_TELEGRAM_CHAT_ID"     // Replace with your chat ID

WiFiClientSecure espClientSecure; // For MQTT TLS
PubSubClient mqttClient(espClientSecure);
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, espClientSecure); // For Telegram TLS

// ==================================================================================================================
// 4. Pin Definitions
// ==================================================================================================================
// DHT Sensor
#define DHTPIN D3     // GPIO0 (D3 on NodeMCU)
#define DHTTYPE DHT11 // DHT11 or DHT22
DHT dht(DHTPIN, DHTTYPE);

// Soil Moisture Sensor (Analog)
#define SOIL_MOISTURE_PIN A0 // A0 on NodeMCU

// Relays (Active LOW assumed, adjust if Active HIGH)
#define PUMP_RELAY_PIN D1   // GPIO5 (D1 on NodeMCU)
#define ZONE1_RELAY_PIN D2  // GPIO4 (D2 on NodeMCU)
#define ZONE2_RELAY_PIN D5  // GPIO14 (D5 on NodeMCU)
#define ZONE3_RELAY_PIN D6  // GPIO12 (D6 on NodeMCU)

// ==================================================================================================================
// 5. Global State Variables
// ==================================================================================================================
// Sensor Readings
float temperature = NAN;
float humidity = NAN;
int soilMoisture = 0; // 0-100%

// Relay States (true = ON, false = OFF)
bool pumpState = false;
bool zoneState[4] = {false, false, false, false}; // zoneState[0] unused, zones 1-3
bool autoWateringEnabled = false;

// Auto Watering Settings
int autoWateringThreshold = 50; // % soil moisture
int autoWateringDuration = 30;  // seconds
int autoWateringInterval = 60;  // minutes
unsigned long lastAutoWateringTime = 0;

// Timers
Ticker wifiReconnectTicker;
Ticker mqttReconnectTicker;
Ticker sensorReadTicker;
Ticker mqttPublishTicker;
Ticker autoWateringTicker;
Ticker watchdogFeedTicker;

// Web Server
ESP8266WebServer server(80);

// RTC
RTC_DS3231 rtc;

// ==================================================================================================================
// 6. Function Prototypes
// ==================================================================================================================
void setupWiFi();
void onWiFiConnect(const WiFiEventStationModeGotIP& event);
void onWiFiDisconnect(const WiFiEventStationModeDisconnected& event);
void setupMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void publishSensorData();
void readSensors();
void setRelayState(int pin, bool state, const char* name, const char* mqttTopic);
void handlePumpCommand(String command);
void handleZoneCommand(int zoneNum, String command);
void handleAutoCommand(String command);
void setupWebServer();
void handleRoot();
void handleNotFound();
void handleApiStatus();
void handleApiSensors();
void handleApiRelay();
void handleApiControl();
void setupOTA();
void sendTelegramMessage(String msg);
void handleTelegramMessages();
void autoWateringCheck();
void saveStateToEEPROM();
void loadStateFromEEPROM();
void feedWatchdog();
void logMessage(String msg);

// ==================================================================================================================
// 7. Setup
// ==================================================================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting Smart Farm PRO Ultimate...");

  // Initialize EEPROM
  EEPROM.begin(512); // Allocate 512 bytes for EEPROM
  loadStateFromEEPROM();

  // Pin Modes
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(ZONE1_RELAY_PIN, OUTPUT);
  pinMode(ZONE2_RELAY_PIN, OUTPUT);
  pinMode(ZONE3_RELAY_PIN, OUTPUT);

  // Restore relay states
  setRelayState(PUMP_RELAY_PIN, pumpState, "Pump", MQTT_TOPIC_PUMP_STATUS);
  setRelayState(ZONE1_RELAY_PIN, zoneState[1], "Zone 1", String(MQTT_TOPIC_ZONE_STATUS).replace("%d", "1").c_str());
  setRelayState(ZONE2_RELAY_PIN, zoneState[2], "Zone 2", String(MQTT_TOPIC_ZONE_STATUS).replace("%d", "2").c_str());
  setRelayState(ZONE3_RELAY_PIN, zoneState[3], "Zone 3", String(MQTT_TOPIC_ZONE_STATUS).replace("%d", "3").c_str());

  // Setup WiFi
  WiFi.mode(WiFiMode_STA);
  WiFi.onStationModeGotIP(onWiFiConnect);
  WiFi.onStationModeDisconnected(onWiFiDisconnect);
  setupWiFi();

  // Setup MQTT
  espClientSecure.setInsecure(); // WARNING: This disables certificate validation. Use for testing only.
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  // Setup DHT Sensor
  dht.begin();

  // Setup RTC
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    logMessage("RTC Error: Not found");
  }
  // Set RTC to compile time if not set or power lost
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    logMessage("RTC: Time set to compile time");
  }

  // Setup OTA
  setupOTA();

  // Setup Web Server
  setupWebServer();

  // Setup Tickers (non-blocking timers)
  sensorReadTicker.attach(5, readSensors); // Read sensors every 5 seconds
  mqttPublishTicker.attach(5, publishSensorData); // Publish sensor data every 5 seconds
  autoWateringTicker.attach(60, autoWateringCheck); // Check auto watering every 60 seconds
  watchdogFeedTicker.attach(2, feedWatchdog); // Feed watchdog every 2 seconds

  Serial.println("Smart Farm PRO Ultimate Ready!");
  logMessage("System: Booted and Ready");
}

// ==================================================================================================================
// 8. Loop
// ==================================================================================================================
void loop() {
  // Handle WiFi and MQTT connections
  if (WiFi.status() != WL_CONNECTED) {
    wifiReconnectTicker.tick();
  }
  if (!mqttClient.connected()) {
    mqttReconnectTicker.tick();
  }

  mqttClient.loop();
  server.handleClient();
  ArduinoOTA.handle();
  handleTelegramMessages();

  // Ticker updates
  sensorReadTicker.tick();
  mqttPublishTicker.tick();
  autoWateringTicker.tick();
  watchdogFeedTicker.tick();
}

// ==================================================================================================================
// 9. WiFi Functions
// ==================================================================================================================
void setupWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  // Use a ticker for non-blocking reconnect attempts
  wifiReconnectTicker.once(10, setupWiFi); // Try again in 10 seconds if not connected
}

void onWiFiConnect(const WiFiEventStationModeGotIP& event) {
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
  logMessage("WiFi: Connected, IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()));
  sendTelegramMessage("✅ WiFi Connected! IP: " + WiFi.localIP().toString());
  wifiReconnectTicker.detach(); // Stop reconnect attempts once connected
  reconnectMQTT(); // Attempt MQTT connection after WiFi is up
}

void onWiFiDisconnect(const WiFiEventStationModeDisconnected& event) {
  Serial.println("WiFi lost connection");
  logMessage("WiFi: Disconnected");
  sendTelegramMessage("❌ WiFi Disconnected! Reconnecting...");
  mqttClient.disconnect(); // Disconnect MQTT if WiFi drops
  wifiReconnectTicker.once(5, setupWiFi); // Try to reconnect WiFi in 5 seconds
}

// ==================================================================================================================
// 10. MQTT Functions
// ==================================================================================================================
void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return; // Only try if WiFi is connected

  Serial.print("Attempting MQTT connection...");
  if (mqttClient.connect(mqtt_client_id.c_str(), mqtt_username, mqtt_password)) {
    Serial.println("connected");
    logMessage("MQTT: Connected");
    sendTelegramMessage("✅ MQTT Connected!");
    mqttClient.subscribe(MQTT_TOPIC_COMMAND);
    // Publish current states after reconnect
    mqttClient.publish(MQTT_TOPIC_PUMP_STATUS, pumpState ? "ON" : "OFF", true);
    for (int i = 1; i <= 3; i++) {
      char topic[30];
      sprintf(topic, MQTT_TOPIC_ZONE_STATUS, i);
      mqttClient.publish(topic, zoneState[i] ? "ON" : "OFF", true);
    }
    mqttClient.publish(MQTT_TOPIC_AUTO_STATUS, autoWateringEnabled ? "ON" : "OFF", true);
    mqttReconnectTicker.detach(); // Stop reconnect attempts once connected
  } else {
    Serial.print("failed, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" trying again in 5 seconds");
    logMessage("MQTT: Connection failed, RC: " + String(mqttClient.state()));
    mqttReconnectTicker.once(5, reconnectMQTT); // Try again in 5 seconds
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("MQTT Message [" + String(topic) + "]: ");
  Serial.println(message);
  logMessage("MQTT Rx: [" + String(topic) + "] " + message);

  String topicStr = String(topic);

  if (topicStr.startsWith("smartfarm/cmd/")) {
    String commandType = topicStr.substring(14); // Remove "smartfarm/cmd/"

    if (commandType.startsWith("pump/")) {
      handlePumpCommand(commandType.substring(5));
    } else if (commandType.startsWith("zone1/")) {
      handleZoneCommand(1, commandType.substring(6));
    } else if (commandType.startsWith("zone2/")) {
      handleZoneCommand(2, commandType.substring(6));
    } else if (commandType.startsWith("zone3/")) {
      handleZoneCommand(3, commandType.substring(6));
    } else if (commandType.startsWith("auto/")) {
      handleAutoCommand(commandType.substring(5));
    }
  }
}

void publishSensorData() {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<256> doc;
  doc["temp"] = String(temperature, 2);
  doc["hum"] = String(humidity, 2);
  doc["soil"] = soilMoisture;
  doc["rtc"] = rtc.now().timestamp(DateTime::TIMESTAMP_TIME);

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  mqttClient.publish(MQTT_TOPIC_SENSOR_DATA, jsonBuffer);
  Serial.print("MQTT Tx: Sensor Data ");
  Serial.println(jsonBuffer);
}

// ==================================================================================================================
// 11. Sensor Functions
// ==================================================================================================================
void readSensors() {
  // DHT Sensor
  float newTemp = dht.readTemperature();
  float newHum = dht.readHumidity();

  if (isnan(newTemp) || isnan(newHum)) {
    Serial.println("Failed to read DHT sensor! Retrying...");
    logMessage("Sensor Error: DHT read failed");
    // Retry logic (DHT library already has some internal retries)
    // For critical applications, consider a small delay and re-read here.
  } else {
    temperature = newTemp;
    humidity = newHum;
  }

  // Soil Moisture Sensor
  // Read analog value (0-1023 for ESP8266 A0)
  int rawSoil = analogRead(SOIL_MOISTURE_PIN);
  // Map raw value to percentage (adjust 0 and 1023 based on your sensor calibration)
  // Example: 0 = dry, 1023 = wet. Invert if 0 = wet, 1023 = dry.
  soilMoisture = map(rawSoil, 0, 1023, 100, 0); // Assuming 0 is wettest, 1023 is driest
  soilMoisture = constrain(soilMoisture, 0, 100); // Ensure it's within 0-100%

  Serial.printf("Sensors: Temp: %.2f C, Hum: %.2f %%, Soil: %d %%, Time: %s\n", 
                temperature, humidity, soilMoisture, rtc.now().timestamp(DateTime::TIMESTAMP_TIME).c_str());
}

// ==================================================================================================================
// 12. Relay Control Functions
// ==================================================================================================================
void setRelayState(int pin, bool state, const char* name, const char* mqttTopic) {
  digitalWrite(pin, state ? HIGH : LOW); // Assuming active HIGH relay
  Serial.printf("%s: %s\n", name, state ? "ON" : "OFF");
  if (mqttClient.connected()) {
    mqttClient.publish(mqttTopic, state ? "ON" : "OFF", true); // Retained message
  }
  logMessage(String(name) + ": " + (state ? "ON" : "OFF"));
  saveStateToEEPROM();
}

void handlePumpCommand(String command) {
  if (command == "on") {
    if (!pumpState) {
      pumpState = true;
      setRelayState(PUMP_RELAY_PIN, pumpState, "Pump", MQTT_TOPIC_PUMP_STATUS);
      sendTelegramMessage("💧 Pump ON by command!");
    }
  } else if (command == "off") {
    if (pumpState) {
      pumpState = false;
      setRelayState(PUMP_RELAY_PIN, pumpState, "Pump", MQTT_TOPIC_PUMP_STATUS);
      sendTelegramMessage("🛑 Pump OFF by command!");
    }
  }
}

void handleZoneCommand(int zoneNum, String command) {
  if (zoneNum < 1 || zoneNum > 3) return;
  char topic[30];
  sprintf(topic, MQTT_TOPIC_ZONE_STATUS, zoneNum);

  if (command == "on") {
    if (!zoneState[zoneNum]) {
      zoneState[zoneNum] = true;
      setRelayState( (zoneNum == 1) ? ZONE1_RELAY_PIN : (zoneNum == 2) ? ZONE2_RELAY_PIN : ZONE3_RELAY_PIN, 
                     zoneState[zoneNum], ("Zone " + String(zoneNum)).c_str(), topic);
      sendTelegramMessage("🌱 Zone " + String(zoneNum) + " ON by command!");
    }
  } else if (command == "off") {
    if (zoneState[zoneNum]) {
      zoneState[zoneNum] = false;
      setRelayState( (zoneNum == 1) ? ZONE1_RELAY_PIN : (zoneNum == 2) ? ZONE2_RELAY_PIN : ZONE3_RELAY_PIN, 
                     zoneState[zoneNum], ("Zone " + String(zoneNum)).c_str(), topic);
      sendTelegramMessage("🍂 Zone " + String(zoneNum) + " OFF by command!");
    }
  }
}

void handleAutoCommand(String command) {
  if (command == "on") {
    if (!autoWateringEnabled) {
      autoWateringEnabled = true;
      Serial.println("Auto Watering ON");
      if (mqttClient.connected()) mqttClient.publish(MQTT_TOPIC_AUTO_STATUS, "ON", true);
      logMessage("Auto Watering: ON");
      sendTelegramMessage("🤖 Auto Watering ON!");
    }
  } else if (command == "off") {
    if (autoWateringEnabled) {
      autoWateringEnabled = false;
      Serial.println("Auto Watering OFF");
      if (mqttClient.connected()) mqttClient.publish(MQTT_TOPIC_AUTO_STATUS, "OFF", true);
      logMessage("Auto Watering: OFF");
      sendTelegramMessage("🤖 Auto Watering OFF!");
    }
  }
  saveStateToEEPROM();
}

// ==================================================================================================================
// 13. Auto Watering Logic
// ==================================================================================================================
void autoWateringCheck() {
  if (!autoWateringEnabled) return;

  DateTime now = rtc.now();
  unsigned long currentMillis = now.unixtime();

  // Check soil moisture threshold
  if (soilMoisture < autoWateringThreshold) {
    // Check if enough time has passed since last auto watering
    if (currentMillis - lastAutoWateringTime > (unsigned long)autoWateringInterval * 60) {
      Serial.println("Auto Watering: Soil too dry, starting pump...");
      logMessage("Auto Watering: Soil dry, pump ON");
      sendTelegramMessage("💦 Auto Watering: ดินแห้ง! เปิดปั๊มอัตโนมัติ.");
      
      handlePumpCommand("on");
      // Set a timer to turn off the pump after autoWateringDuration
      autoWateringTicker.once(autoWateringDuration, [](){
        handlePumpCommand("off");
        sendTelegramMessage("💧 Auto Watering: ปิดปั๊มอัตโนมัติ (ครบเวลา).");
        logMessage("Auto Watering: Pump OFF (duration complete)");
      });
      lastAutoWateringTime = currentMillis;
      saveStateToEEPROM();
    }
  }
  // Add RTC schedule logic here if needed (e.g., water at specific times regardless of soil)
}

// ==================================================================================================================
// 14. Dashboard API (HTTP Server) Functions
// ==================================================================================================================
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/sensors", HTTP_GET, handleApiSensors);
  server.on("/api/relay", HTTP_GET, handleApiRelay);
  server.on("/api/control", HTTP_POST, handleApiControl);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
  logMessage("HTTP: Server started");
}

void handleRoot() {
  server.send(200, "text/plain", "Smart Farm PRO Ultimate API. Use /api/status, /api/sensors, /api/relay, /api/control");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

void handleApiStatus() {
  StaticJsonDocument<256> doc;
  doc["wifi_status"] = (WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected";
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["mqtt_status"] = mqttClient.connected() ? "Connected" : "Disconnected";
  doc["pump_state"] = pumpState ? "ON" : "OFF";
  doc["zone1_state"] = zoneState[1] ? "ON" : "OFF";
  doc["zone2_state"] = zoneState[2] ? "ON" : "OFF";
  doc["zone3_state"] = zoneState[3] ? "ON" : "OFF";
  doc["auto_watering_enabled"] = autoWateringEnabled ? "ON" : "OFF";
  doc["uptime_seconds"] = millis() / 1000;

  String jsonResponse;
  serializeJson(doc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

void handleApiSensors() {
  StaticJsonDocument<256> doc;
  doc["temperature"] = String(temperature, 2);
  doc["humidity"] = String(humidity, 2);
  doc["soil_moisture"] = soilMoisture;
  doc["rtc_time"] = rtc.now().timestamp(DateTime::TIMESTAMP_TIME);

  String jsonResponse;
  serializeJson(doc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

void handleApiRelay() {
  StaticJsonDocument<256> doc;
  doc["pump"] = pumpState ? "ON" : "OFF";
  doc["zone1"] = zoneState[1] ? "ON" : "OFF";
  doc["zone2"] = zoneState[2] ? "ON" : "OFF";
  doc["zone3"] = zoneState[3] ? "ON" : "OFF";

  String jsonResponse;
  serializeJson(doc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

void handleApiControl() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Bad Request: Missing JSON body");
    return;
  }

  String body = server.arg("plain");
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    server.send(400, "text/plain", "Bad Request: Invalid JSON");
    return;
  }

  String device = doc["device"].as<String>();
  String command = doc["command"].as<String>();
  int zone = doc["zone"].as<int>(); // For zone control

  bool success = false;
  if (device == "pump") {
    handlePumpCommand(command);
    success = true;
  } else if (device == "zone") {
    handleZoneCommand(zone, command);
    success = true;
  } else if (device == "auto") {
    handleAutoCommand(command);
    success = true;
  }

  if (success) {
    server.send(200, "application/json", "{\"status\":\"OK\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"Error\", \"message\":\"Invalid device or command\"}");
  }
}

// ==================================================================================================================
// 15. OTA (Over-The-Air) Update Functions
// ==================================================================================================================
void setupOTA() {
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.get== OTA_UPDATE_BEGIN) {
      type = "firmware";
    } else {
      type = "filesystem";
    }
    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
    logMessage("OTA: Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    logMessage("OTA: Update End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    logMessage("OTA Error: " + String(error));
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Initialized");
  logMessage("OTA: Initialized");
}

// ==================================================================================================================
// 16. Telegram Notification Functions
// ==================================================================================================================
void sendTelegramMessage(String msg) {
  if (TELEGRAM_BOT_TOKEN[0] == 'Y' && TELEGRAM_CHAT_ID[0] == 'Y') { // Check if placeholders are replaced
    Serial.println("Telegram: Bot token or chat ID not set. Skipping message.");
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    bot.sendMessage(TELEGRAM_CHAT_ID, msg, "Markdown");
    Serial.print("Telegram Tx: ");
    Serial.println(msg);
  } else {
    Serial.println("Telegram: WiFi not connected, cannot send message.");
  }
}

void handleTelegramMessages() {
  if (millis() > bot.lastUpdateTime + 1000) { // Check for new messages every second
    int numNewMessages = bot.getUpdates(bot.lastUpdateTime + 1);
    while (numNewMessages) {
      Serial.println("Telegram: Got " + String(numNewMessages) + " new messages");
      for (int i = 0; i < numNewMessages; i++) {
        String chat_id = String(bot.messages[i].chat_id);
        String text = bot.messages[i].text;
        Serial.println("Telegram Rx: [" + chat_id + "] " + text);
        logMessage("Telegram Rx: [" + chat_id + "] " + text);

        if (chat_id == TELEGRAM_CHAT_ID) { // Only process messages from authorized chat ID
          if (text == "/status") {
            String statusMsg = "*Smart Farm Status*\n";
            statusMsg += "WiFi: " + String((WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected") + " (RSSI: " + String(WiFi.RSSI()) + ")\n";
            statusMsg += "MQTT: " + String(mqttClient.connected() ? "Connected" : "Disconnected") + "\n";
            statusMsg += "Pump: " + String(pumpState ? "ON" : "OFF") + "\n";
            statusMsg += "Zone 1: " + String(zoneState[1] ? "ON" : "OFF") + "\n";
            statusMsg += "Zone 2: " + String(zoneState[2] ? "ON" : "OFF") + "\n";
            statusMsg += "Zone 3: " + String(zoneState[3] ? "ON" : "OFF") + "\n";
            statusMsg += "Auto Watering: " + String(autoWateringEnabled ? "ON" : "OFF") + "\n";
            statusMsg += String(temperature, 2) + "°C, " + String(humidity, 2) + "% RH, " + String(soilMoisture) + "% Soil\n";
            statusMsg += "Time: " + rtc.now().timestamp(DateTime::TIMESTAMP_FULL) + "\n";
            bot.sendMessage(chat_id, statusMsg, "Markdown");
          } else if (text == "/pump_on") {
            handlePumpCommand("on");
            bot.sendMessage(chat_id, "Pump ON command sent.");
          } else if (text == "/pump_off") {
            handlePumpCommand("off");
            bot.sendMessage(chat_id, "Pump OFF command sent.");
          } else if (text == "/zone1_on") {
            handleZoneCommand(1, "on");
            bot.sendMessage(chat_id, "Zone 1 ON command sent.");
          } else if (text == "/zone1_off") {
            handleZoneCommand(1, "off");
            bot.sendMessage(chat_id, "Zone 1 OFF command sent.");
          } else if (text == "/zone2_on") {
            handleZoneCommand(2, "on");
            bot.sendMessage(chat_id, "Zone 2 ON command sent.");
          } else if (text == "/zone2_off") {
            handleZoneCommand(2, "off");
            bot.sendMessage(chat_id, "Zone 2 OFF command sent.");
          } else if (text == "/zone3_on") {
            handleZoneCommand(3, "on");
            bot.sendMessage(chat_id, "Zone 3 ON command sent.");
          } else if (text == "/zone3_off") {
            handleZoneCommand(3, "off");
            bot.sendMessage(chat_id, "Zone 3 OFF command sent.");
          } else if (text == "/auto_on") {
            handleAutoCommand("on");
            bot.sendMessage(chat_id, "Auto Watering ON command sent.");
          } else if (text == "/auto_off") {
            handleAutoCommand("off");
            bot.sendMessage(chat_id, "Auto Watering OFF command sent.");
          } else if (text == "/restart") {
            bot.sendMessage(chat_id, "Restarting ESP...");
            logMessage("System: Restarting by Telegram command");
            ESP.restart();
          } else {
            bot.sendMessage(chat_id, "Unknown command. Try /status, /pump_on, /pump_off, etc.");
          }
        }
      }
      bot.lastUpdateTime = bot.messages[numNewMessages - 1].date; // Update last update time
      numNewMessages = bot.getUpdates(bot.lastUpdateTime + 1); // Get next batch
    }
  }
}

// ==================================================================================================================
// 17. Reliability & Persistence Functions
// ==================================================================================================================
// Watchdog: ESP.wdtFeed() is called by loop() implicitly for ESP8266. Explicit call for safety.
void feedWatchdog() {
  ESP.wdtFeed();
}

// EEPROM Structure for saving state
struct { 
  bool pumpState;
  bool zoneState[4];
  bool autoWateringEnabled;
  unsigned long lastAutoWateringTime;
} persistentState;

void saveStateToEEPROM() {
  persistentState.pumpState = pumpState;
  for(int i=0; i<4; i++) persistentState.zoneState[i] = zoneState[i];
  persistentState.autoWateringEnabled = autoWateringEnabled;
  persistentState.lastAutoWateringTime = lastAutoWateringTime;

  EEPROM.put(0, persistentState);
  EEPROM.commit();
  Serial.println("State saved to EEPROM.");
  logMessage("State: Saved to EEPROM");
}

void loadStateFromEEPROM() {
  EEPROM.get(0, persistentState);
  // Check for initial EEPROM values (all 0xFF if not written before)
  if (persistentState.pumpState == 0xFF) {
    Serial.println("EEPROM empty or invalid, initializing default state.");
    pumpState = false;
    for(int i=0; i<4; i++) zoneState[i] = false;
    autoWateringEnabled = false;
    lastAutoWateringTime = 0;
    saveStateToEEPROM(); // Save defaults
  } else {
    pumpState = persistentState.pumpState;
    for(int i=0; i<4; i++) zoneState[i] = persistentState.zoneState[i];
    autoWateringEnabled = persistentState.autoWateringEnabled;
    lastAutoWateringTime = persistentState.lastAutoWateringTime;
    Serial.println("State loaded from EEPROM.");
  }
  logMessage("State: Loaded from EEPROM");
}

// Simple logging to Serial and MQTT
void logMessage(String msg) {
  Serial.print("LOG: ");
  Serial.println(msg);
  if (mqttClient.connected()) {
    mqttClient.publish(MQTT_TOPIC_LOG, msg.c_str());
  }
}

// ==================================================================================================================
// 18. Final Verification Notes
// ==================================================================================================================
// - Replace YOUR_WIFI_SSID, YOUR_WIFI_PASSWORD, YOUR_TELEGRAM_BOT_TOKEN, YOUR_TELEGRAM_CHAT_ID
// - Adjust DHTPIN, DHTTYPE, SOIL_MOISTURE_PIN, RELAY_PINS based on your hardware.
// - Relay logic (HIGH/LOW) might need inversion depending on your relay module (active HIGH vs active LOW).
// - Soil moisture mapping (0-1023 to 0-100%) needs calibration for your specific sensor and soil conditions.
// - RTC setup assumes DS3231. If using another RTC, adjust RTClib usage.
// - Telegram bot commands are basic. Expand as needed.
// - The `espClientSecure.setInsecure()` is for development/testing. For production, proper certificate validation is recommended.
// - The `ArduinoJson` library is used for sensor data. Ensure it's installed (e.g., v6).
// - `UniversalTelegramBot` library is used. Ensure it's installed.
// - `RTClib` library is used. Ensure it's installed.
// - `DHT` library is used. Ensure it's installed.
