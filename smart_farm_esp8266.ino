#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// MQTT Broker details (HiveMQ Cloud)
const char* mqtt_server = "650188a0ee2b4367b7c131fb385590a9.s1.eu.hivemq.cloud";
const int mqtt_port = 8884; 
const char* mqtt_username = "smartfarm";
const char* mqtt_password = "Kla12345";
const char* mqtt_client_id = "ESP8266_Pump_Control"; 

// MQTT Topics
const char* mqtt_topic_pump_control = "farm/pump/control";
const char* mqtt_topic_pump_status = "farm/pump/status";

// Pin definitions
const int PUMP_RELAY_PIN = D2; // ขาควบคุมรีเลย์ปั๊มน้ำ

WiFiClient espClient;
PubSubClient client(espClient);

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [" + String(topic) + "]: ");
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  if (String(topic) == mqtt_topic_pump_control) {
    if (message == "ON") {
      digitalWrite(PUMP_RELAY_PIN, HIGH); // เปิดปั๊ม
      client.publish(mqtt_topic_pump_status, "ON");
      Serial.println("Pump turned ON");
    } else if (message == "OFF") {
      digitalWrite(PUMP_RELAY_PIN, LOW); // ปิดปั๊ม
      client.publish(mqtt_topic_pump_status, "OFF");
      Serial.println("Pump turned OFF");
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect(mqtt_client_id, mqtt_username, mqtt_password)) {
      Serial.println("connected");
      client.subscribe(mqtt_topic_pump_control);
      // ส่งสถานะปัจจุบันเมื่อเชื่อมต่อสำเร็จ
      client.publish(mqtt_topic_pump_status, (digitalRead(PUMP_RELAY_PIN) == HIGH) ? "ON" : "OFF");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW); // ปิดปั๊มไว้ก่อนในตอนเริ่ม

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
}
