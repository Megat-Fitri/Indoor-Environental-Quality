#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Preferences.h>

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
Preferences preferences;

// ==========================================
// NETWORK CONFIGURATION
// ==========================================
const char* STATION_SSID = "HONOR X9c Smart";
const char* STATION_PASSWORD = "blablabla";
const char* serverBaseUrl = "http://10.95.162.48:3000";

String deviceId = "ESP32_ROOM_1";
String alertMode = "AUTO";
float tempThreshold = 30.0f;
float humidityMin = 40.0f;
float humidityMax = 70.0f;
int noiseThreshold = 500;
int gasWarningThreshold = 300;
int gasCriticalThreshold = 500;
int uploadInterval = 10;

// Data Array Averaging Ring Buffer
const int TotalReadingsCount = 10;
float arrayTemp[TotalReadingsCount];
float arrayHumid[TotalReadingsCount];
int arraySound[TotalReadingsCount];
int arrayGas[TotalReadingsCount];
int indexPosition = 0;
bool collectionFilled = false;

float averageTemp = 0.0f;
float averageHumid = 0.0f;
int averageSound = 0;
int averageGas = 0;

unsigned long lastAverageMillis = 0;
unsigned long lastSettingsDownloadMillis = 0;
unsigned long lastUploadTime = 0;
unsigned long lastButtonPress = 0;
int displayMode = 0; 

int buzzerSequenceStep = 0;
unsigned long lastBuzzerActionMillis = 0;

TaskHandle_t UploadTaskHandle = NULL;

void calculateAverages();
void readSensors();
void updateBuzzer();
void updateOLED();
bool downloadSettings();
void saveSettings();
void loadCachedSettings();
bool evaluateComfort();
String evaluateSafety();
void maintainWiFi();
void telemetryWorkerTask(void *pvParameters);

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();

  // Initializing the Wire interface explicitly to guarantee correct screen allocation boundaries
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Syncing System..."));
  display.display();

  loadCachedSettings();

  WiFi.begin(STATION_SSID, STATION_PASSWORD);
  Serial.print(F("Connecting to WiFi"));
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(F("."));
  }
  Serial.println(F("\nWiFi Connected successfully!"));

  downloadSettings();

  xTaskCreatePinnedToCore(telemetryWorkerTask, "TelemetryTask", 8192, NULL, 1, &UploadTaskHandle, 1);
}

void loop() {
  unsigned long currentMillis = millis();

  readSensors();
  if (currentMillis - lastAverageMillis >= 1000) {
    calculateAverages();
    lastAverageMillis = currentMillis;
  }

  if (digitalRead(BUTTON_PIN) == LOW) {
    if (currentMillis - lastButtonPress >= 250) {
      displayMode = (displayMode + 1) % 2;
      lastButtonPress = currentMillis;
    }
  }

  maintainWiFi();

  // Checks server configuration updates dynamically every 2 seconds
  if (currentMillis - lastSettingsDownloadMillis >= 2000) {
    lastSettingsDownloadMillis = currentMillis;
    downloadSettings();
  }

  updateBuzzer();
  updateOLED();

  if (currentMillis - lastUploadTime >= ((unsigned long)uploadInterval * 1000)) {
    lastUploadTime = currentMillis;
    if (UploadTaskHandle != NULL) {
      xTaskNotifyGive(UploadTaskHandle);
    }
  }
}

void readSensors() {
  float currentT = dht.readTemperature();
  float currentH = dht.readHumidity();
  if (!isnan(currentT)) arrayTemp[indexPosition] = currentT;
  if (!isnan(currentH)) arrayHumid[indexPosition] = currentH;
  arraySound[indexPosition] = analogRead(SOUNDPIN);
  arrayGas[indexPosition] = analogRead(MQ2PIN);

  indexPosition++;
  if (indexPosition >= TotalReadingsCount) {
    indexPosition = 0;
    collectionFilled = true;
  }
}

void calculateAverages() {
  int bounds = collectionFilled ? TotalReadingsCount : indexPosition;
  if (bounds == 0) return;
  float sumT = 0, sumH = 0; long sumS = 0, sumG = 0;
  for (int i = 0; i < bounds; i++) {
    sumT += arrayTemp[i]; sumH += arrayHumid[i]; sumS += arraySound[i]; sumG += arrayGas[i];
  }
  averageTemp = sumT / bounds;
  averageHumid = sumH / bounds;
  averageSound = (int)(sumS / bounds);
  averageGas = (int)(sumG / bounds);
}

void maintainWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(STATION_SSID, STATION_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) { delay(500); attempts++; }
  }
}

bool downloadSettings() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String endpoint = String(serverBaseUrl) + "/api/settings?device_id=" + deviceId;
  http.begin(endpoint);
  http.setTimeout(3000);

  int httpResponseCode = http.GET();
  if (httpResponseCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, http.getStream());
  if (error) {
    Serial.print(F("JSON Parse Fault: "));
    Serial.println(error.f_str());
    http.end();
    return false;
  }
  http.end();

  // Safely map across both variable names to ensure the thresholds update flawlessly
  if (doc.containsKey("temp_threshold")) tempThreshold = doc["temp_threshold"].as<float>();
  if (doc.containsKey("humidity_min")) humidityMin = doc["humidity_min"].as<float>();
  if (doc.containsKey("humidity_max")) humidityMax = doc["humidity_max"].as<float>();
  if (doc.containsKey("noise_threshold")) noiseThreshold = doc["noise_threshold"].as<int>();
  
  if (doc.containsKey("gas_warning_threshold")) gasWarningThreshold = doc["gas_warning_threshold"].as<int>();
  else if (doc.containsKey("gas_warning")) gasWarningThreshold = doc["gas_warning"].as<int>();

  if (doc.containsKey("gas_critical_threshold")) gasCriticalThreshold = doc["gas_critical_threshold"].as<int>();
  else if (doc.containsKey("gas_critical")) gasCriticalThreshold = doc["gas_critical"].as<int>();
  
  if (doc.containsKey("upload_interval")) {
    uploadInterval = doc["upload_interval"].as<int>();
    if (uploadInterval < 2) uploadInterval = 2;
  }
  if (doc.containsKey("alert_mode")) {
    String serverAlertMode = doc["alert_mode"].as<const char*>();
    serverAlertMode.toUpperCase();
    if (serverAlertMode == "AUTO" || serverAlertMode == "MUTE") alertMode = serverAlertMode;
  }

  Serial.printf("Config Synchronized -> Temp Thresh: %.1f, Gas Warning: %d, Gas Critical: %d\n", 
                tempThreshold, gasWarningThreshold, gasCriticalThreshold);

  saveSettings();
  return true;
}

void saveSettings() {
  preferences.begin("ieq_config", false);
  preferences.putFloat("tThresh", tempThreshold);
  preferences.putFloat("hMin", humidityMin);
  preferences.putFloat("hMax", humidityMax);
  preferences.putInt("nThresh", noiseThreshold);
  preferences.putInt("gWarn", gasWarningThreshold);
  preferences.putInt("gCrit", gasCriticalThreshold);
  preferences.putInt("uInterval", uploadInterval);
  preferences.putString("aMode", alertMode);
  preferences.end();
}

void loadCachedSettings() {
  preferences.begin("ieq_config", true);
  tempThreshold = preferences.getFloat("tThresh", 30.0f);
  humidityMin = preferences.getFloat("hMin", 40.0f);
  humidityMax = preferences.getFloat("hMax", 70.0f);
  noiseThreshold = preferences.getInt("nThresh", 500);
  gasWarningThreshold = preferences.getInt("gWarn", 300);
  gasCriticalThreshold = preferences.getInt("gCrit", 500);
  uploadInterval = preferences.getInt("uInterval", 10);
  alertMode = preferences.getString("aMode", "AUTO");
  preferences.end();
}

// FIXED: Removed the faulty `averageTemp >= 20.0f` hardcoded lower lockout check
bool evaluateComfort() {
  return (averageTemp <= tempThreshold && 
          averageHumid >= humidityMin && 
          averageHumid <= humidityMax && 
          averageSound <= noiseThreshold);
}

String evaluateSafety() {
  if (averageGas > gasCriticalThreshold) return "CRITICAL";
  if (averageGas > gasWarningThreshold) return "WARNING";
  return "NORMAL";
}

void updateBuzzer() {
  unsigned long now = millis();
  String safetyStatus = evaluateSafety();
  bool comfortStatus = evaluateComfort();

  // If Muted or if conditions are clean and comfortable, stop beeping entirely!
  if (alertMode != "AUTO" || (safetyStatus == "NORMAL" && comfortStatus)) {
    noTone(BUZZER_PIN);
    buzzerSequenceStep = 0;
    return;
  }

  if (safetyStatus == "CRITICAL") {
    if (buzzerSequenceStep == 0) { tone(BUZZER_PIN, 2000); lastBuzzerActionMillis = now; buzzerSequenceStep = 1; }
    else if (buzzerSequenceStep == 1 && (now - lastBuzzerActionMillis >= 150)) { noTone(BUZZER_PIN); lastBuzzerActionMillis = now; buzzerSequenceStep = 2; }
    else if (buzzerSequenceStep == 2 && (now - lastBuzzerActionMillis >= 150)) { buzzerSequenceStep = 0; }
  } else {
    int intervalTime = (safetyStatus == "WARNING") ? 1000 : 2000;
    if (buzzerSequenceStep == 0) { tone(BUZZER_PIN, 1000); lastBuzzerActionMillis = now; buzzerSequenceStep = 1; }
    else if (buzzerSequenceStep == 1 && (now - lastBuzzerActionMillis >= 400)) { noTone(BUZZER_PIN); lastBuzzerActionMillis = now; buzzerSequenceStep = 2; }
    else if (buzzerSequenceStep == 2 && (now - lastBuzzerActionMillis >= intervalTime)) { buzzerSequenceStep = 0; }
  }
}

void updateOLED() {
  display.clearDisplay(); 
  display.setCursor(0, 0);
  
  display.println(WiFi.status() == WL_CONNECTED ? "[ ONLINE - IEQ HUB ]" : "[ OFFLINE SYNC.. ]");
  display.println("---------------------");

  if (displayMode == 0) {
    display.print("Temp:  "); display.print(averageTemp, 1); display.println(" C");
    display.print("Humid: "); display.print(averageHumid, 1); display.println(" %");
    display.print("Noise: "); display.println(averageSound);
    display.println("---------------------");
    display.print("State: "); display.println(evaluateComfort() ? "COMFORT" : "UNCOMFORTABLE");
  } else {
    display.print("Gas Smoke: "); display.println(averageGas);
    display.println("---------------------");
    display.print("SAFETY: "); display.println(evaluateSafety());
  }
  display.display();
}

void telemetryWorkerTask(void *pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(String(serverBaseUrl) + "/api/sensor-data");
      http.addHeader("Content-Type", "application/json");

      StaticJsonDocument<256> outDoc;
      outDoc["device_id"] = deviceId;
      outDoc["temperature"] = averageTemp;
      outDoc["humidity"] = averageHumid;
      outDoc["sound_level"] = averageSound;
      outDoc["air_quality"] = averageGas;

      String body; serializeJson(outDoc, body);
      http.POST(body); http.end();
    }
  }
}