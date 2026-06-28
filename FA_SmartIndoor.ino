#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ==========================================
// PIN CONFIGURATION
// ==========================================
#define DHTPIN 4
#define DHTTYPE DHT11
#define MQ2PIN 34
#define SOUNDPIN 35
#define BUTTON_PIN 13
#define BUZZER_PIN 18

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ==========================================
// NETWORK AND ENDPOINT TARGET LINKS
// ==========================================
const char* STATION_SSID = "HONOR X9c Smart";         
const char* STATION_PASSWORD = "blablabla"; 
const char* serverUrl = "http://10.253.86.48:3000/api/sensor-data"; 

String deviceId = "ESP32_ROOM_1";
float tempThreshold = 32.0;
int gasThreshold = 400;
int uploadInterval = 10; 
unsigned long lastUploadTime = 0;

int displayMode = 0; // 0 = Environment (Comfort), 1 = Safety (Gas)

// ==========================================
// ADVANCED BUZZER PATTERN SEQUENCE ENGINE
// ==========================================
unsigned long lastBuzzerTick = 0;
int buzzerSequenceStep = 0;

void setup() {
  Serial.begin(115200);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED Panel allocation failed."));
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,10);
  display.println("Booting System...");
  display.println("Connecting to WiFi...");
  display.display();

  Serial.print("Connecting to SSID: ");
  Serial.println(STATION_SSID);

  WiFi.begin(STATION_SSID, STATION_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi Connected Successfully!");
  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.setCursor(0,10);
  display.println("Network Connected!");
  display.print("IP: "); display.println(WiFi.localIP());
  display.display();
  delay(2000);
}

void loop() {
  // 1. Collect real-time instant data from sensors
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  int gasLevel = analogRead(MQ2PIN);
  int soundLevel = analogRead(SOUNDPIN);

  if (isnan(h) || isnan(t)) { t = 0.0; h = 0.0; }

  // 2. Read screen page navigation inputs
  if (digitalRead(BUTTON_PIN) == LOW) {
    displayMode = (displayMode + 1) % 2;
    delay(250); 
  }

  // 3. Comfort Evaluation Criteria Matrix
  bool isComfortable = true;
  if (t > 30.0 || t < 20.0 || h < 40.0 || h > 70.0 || soundLevel > 500) {
    isComfortable = false; 
  }

  // Gas level status logic checks
  String safetyStatus = "NORMAL";
  if (gasLevel > 500) safetyStatus = "CRITICAL";
  else if (gasLevel > 300) safetyStatus = "WARNING";

  // 4. State-Machine Driven Non-Blocking Buzzer Engine
  unsigned long currentMillis = millis();

  if (safetyStatus == "CRITICAL") {
    // CRITICAL: Rapid, nonstop fire beeping (100ms on, 100ms off)
    if (currentMillis - lastBuzzerTick >= 100) {
      lastBuzzerTick = currentMillis;
      buzzerSequenceStep = (buzzerSequenceStep + 1) % 2;
      digitalWrite(BUZZER_PIN, (buzzerSequenceStep == 1) ? HIGH : LOW);
    }
  } 
  else if (safetyStatus == "WARNING") {
    // WARNING: Double beep pattern repeating every 1 second
    // We break 1000ms into 10 steps of 100ms
    if (currentMillis - lastBuzzerTick >= 100) {
      lastBuzzerTick = currentMillis;
      buzzerSequenceStep = (buzzerSequenceStep + 1) % 10;
      
      if (buzzerSequenceStep == 0 || buzzerSequenceStep == 2) {
        digitalWrite(BUZZER_PIN, HIGH); // Beep 1 and Beep 2
      } else {
        digitalWrite(BUZZER_PIN, LOW);  // Silences remaining window slots
      }
    }
  } 
  else if (!isComfortable) {
    // UNCOMFORTABLE: One single beep every second
    if (currentMillis - lastBuzzerTick >= 100) {
      lastBuzzerTick = currentMillis;
      buzzerSequenceStep = (buzzerSequenceStep + 1) % 10;
      
      if (buzzerSequenceStep == 0) {
        digitalWrite(BUZZER_PIN, HIGH); // Single 100ms chirp at start of second
      } else {
        digitalWrite(BUZZER_PIN, LOW);
      }
    }
  } 
  else {
    // ALL SAFE: Quiet down completely
    digitalWrite(BUZZER_PIN, LOW);
    buzzerSequenceStep = 0; 
  }

  // 5. Update OLED Display Panel
  display.clearDisplay();
  display.setCursor(0,0);
  
  if (displayMode == 0) {
    display.println("= Environment Monitor =");
    display.println("---------------------");
    display.print("Temp:  "); display.print(t, 1); display.println(" C");
    display.print("Humid: "); display.print(h, 1); display.println(" %");
    display.print("Noise: "); display.println(soundLevel);
    display.println("---------------------");
    display.print("State: "); display.println(isComfortable ? "COMFORT" : "UNCOMFORTABLE");
  } else {
    display.println("=== SAFETY MODULE ===");
    display.println("---------------------");
    display.print("Gas Smoke Value: "); display.println(gasLevel);
    display.println("---------------------");
    display.print("SAFETY STATE: "); display.println(safetyStatus);
    if (safetyStatus == "CRITICAL") {
      display.println("\n!! EVACUATE AREA !!");
    }
  }
  display.display();

  // 6. Push Packets to Server Framework
  if (currentMillis - lastUploadTime > (uploadInterval * 1000)) {
    lastUploadTime = currentMillis;
    
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(serverUrl);
      http.addHeader("Content-Type", "application/json");

      StaticJsonDocument<250> doc;
      doc["device_id"] = deviceId;
      doc["temperature"] = t;   
      doc["humidity"] = h;
      doc["sound_level"] = soundLevel; 
      doc["air_quality"] = gasLevel;

      String jsonPayload;
      serializeJson(doc, jsonPayload);
      
      Serial.println("\n--- Attempting Data Transmission ---");
      Serial.print("Target Endpoint: "); Serial.println(serverUrl);
      Serial.print("Payload Data: "); Serial.println(jsonPayload);

      int httpResponseCode = http.POST(jsonPayload);
      
      Serial.print("Server Feedback Status Code: ");
      Serial.println(httpResponseCode);
      
      if (httpResponseCode > 0) {
        String responseBody = http.getString();
        Serial.print("Server Response Details: ");
        Serial.println(responseBody);
      } else {
        Serial.println("Transmission Fault: Unable to interface with local server framework.");
      }
      Serial.println("------------------------------------");

      http.end();
    } else {
      Serial.println("Transmission Skipped: Network connection dropped.");
    }
  }
}