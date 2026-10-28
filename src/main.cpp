#include <Arduino.h>
#include "DHT.h"
#include "model_weights.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

#define MOISTURE_PIN 34
#define DHT_PIN 4
#define DHT_TYPE DHT22
#define RAIN_BUTTON_PIN 5
#define LED_RED_PIN 25
#define LED_YELLOW_PIN 26
#define LED_GREEN_PIN 27
#define WARNING_THRESHOLD 0.15
#define BUZZER_PIN 33

// Since we don't have a soil-type sensor, this is set once per plot,
// just like a real installer would configure it. Change this to test
// different soil types: 0 = sandy, 1 = loamy, 2 = clay
#define SOIL_TYPE_INDEX 0

// Lower than the standard 0.5 cutoff, on purpose: a false "irrigate"
// alert costs a bit of wasted water; a missed warning risks the crop.
#define IRRIGATE_THRESHOLD 0.35

DHT dht(DHT_PIN, DHT_TYPE);

unsigned long lastRainTime = 0;
bool lastButtonState = HIGH;
bool lastIrrigateState = false;
bool redLedState = false;
const float SIMULATED_HOURS_PER_REAL_MINUTE = 24.0;

// The model's "brain", hand-ported from the trained Keras model

float relu(float x) {
  return x > 0 ? x : 0;   // ReLU: keep positive numbers, zero-out negative ones
}

float sigmoid(float x) {
  return 1.0 / (1.0 + exp(-x));   // squashes any number into a 0-1 probability

}

// Takes the 7 raw feature values, returns a probability (0.0 to 1.0)
// of "irrigation needed within 24 hours"
float predict_irrigation(float rawFeatures[N_FEATURES]) {
  // Step 1: normalizing each feature using the training mean/std
  float x[N_FEATURES];
  for (int i = 0; i < N_FEATURES; i++) {
    x[i] = (rawFeatures[i] - FEATURE_MEAN[i]) / FEATURE_STD[i];
  }

  // Step 2: Layer 1 (7 inputs -> 12 hidden units), then ReLU
  float h1[N_HIDDEN1];
  for (int j = 0; j < N_HIDDEN1; j++) {
    float sum = BIAS1[j];
    for (int i = 0; i < N_FEATURES; i++) {
      sum += x[i] * W1[i][j];
    }
    h1[j] = relu(sum);
  }

  // Step 3: Layer 2 (12 -> 8 hidden units), then ReLU
  float h2[N_HIDDEN2];
  for (int j = 0; j < N_HIDDEN2; j++) {
    float sum = BIAS2[j];
    for (int i = 0; i < N_HIDDEN1; i++) {
      sum += h1[i] * W2[i][j];
    }
    h2[j] = relu(sum);
  }

  // Step 4: Layer 3 (8 -> 1 output), then Sigmoid

  float sum = BIAS3[0];
  for (int i = 0; i < N_HIDDEN2; i++) {
    sum += h2[i] * W3[i][0];
  }
  return sigmoid(sum);
}


void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.println("Setting up DNS servers...");
  IPAddress dns1(8, 8, 8, 8);
  IPAddress dns2(8, 8, 4, 4);
  Serial.println("Configuring static IP...");
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println("Starting HydroSense...");
  dht.begin();
  pinMode(RAIN_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
}

String urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;   // letters and numbers are safe
    } else {
      // everything else gets converted to %XX (its character code in hex)
      char code0 = ((c >> 4) & 0xf) + '0';
      if (((c >> 4) & 0xf) > 9) code0 = ((c >> 4) & 0xf) - 10 + 'A';
      char code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

void sendSMS(String message) {
  HTTPClient http;

  String url = "https://api.sandbox.africastalking.com/version1/messaging";
  http.begin(url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Accept", "application/json");
  http.addHeader("apiKey", AT_API_KEY);

  String postData = "username=sandbox";
  postData += "&to=" + urlEncode(RECIPIENT_PHONE);  // to test phone number
  postData += "&message=" + urlEncode(message);

  int httpResponseCode = http.POST(postData);
  if (httpResponseCode <= 0) {
    Serial.println("First attempt failed, retrying once...");
    httpResponseCode = http.POST(postData);
  }

  Serial.print("SMS send response code: ");
  Serial.println(httpResponseCode);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Response: " + response);
  }

  http.end();
}

void loop() {
  bool buttonState = digitalRead(RAIN_BUTTON_PIN);
  if (buttonState == LOW && lastButtonState == HIGH) {
    lastRainTime = millis();
    Serial.println(">>> Rain event triggered! Resetting days_since_rain to 0.");
  }
  lastButtonState = buttonState;

  unsigned long millisSinceRain = millis() - lastRainTime;
  float realMinutesSinceRain = millisSinceRain / 60000.0;
  float simulatedHoursSinceRain = realMinutesSinceRain * SIMULATED_HOURS_PER_REAL_MINUTE;
  float daysSinceRain = simulatedHoursSinceRain / 24.0;

  int rawValue = analogRead(MOISTURE_PIN);
  float moisturePercent = (rawValue / 4095.0) * 100.0;


  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  // Building the 7-value feature array, in the EXACT order the model expects
  float features[N_FEATURES];
  features[0] = moisturePercent;
  features[1] = temperature;
  features[2] = humidity;
  features[3] = daysSinceRain;
  features[4] = (SOIL_TYPE_INDEX == 0) ? 1.0 : 0.0;  // soil_sandy
  features[5] = (SOIL_TYPE_INDEX == 1) ? 1.0 : 0.0;  // soil_loamy
  features[6] = (SOIL_TYPE_INDEX == 2) ? 1.0 : 0.0;  // soil_clay

  float probability = predict_irrigation(features);
  bool shouldIrrigate = probability >= IRRIGATE_THRESHOLD;

  if (shouldIrrigate && !lastIrrigateState) {
    String smsText = "HydroSense Alert: Soil moisture at " + String(moisturePercent, 1) +
                      "%. Irrigation recommended within 24h.";
    sendSMS(smsText);

    Serial.println("BEEP triggered now!");   // debug line - temporary
    ledcSetup(0, 1000, 8);         // channel 0, frequency 1000Hz, 8-bit resolution
    ledcAttachPin(BUZZER_PIN, 0);  // attach buzzer pin to channel 0
    ledcWriteTone(0, 1000);        // start the tone on channel 0
    delay(300);
    ledcWriteTone(0, 0);           // stop the tone
  }
  lastIrrigateState = shouldIrrigate;

  // LED indicator logic
  if (shouldIrrigate) {
    // blink red: since loop only repeats every 2 seconds, this toggles
    // on/off once per cycle; a slow blink, not instant, but clearly visible
    redLedState = !redLedState;
    digitalWrite(LED_RED_PIN, redLedState ? HIGH : LOW);
    digitalWrite(LED_YELLOW_PIN, LOW);
    digitalWrite(LED_GREEN_PIN, LOW);
  } else if (probability >= WARNING_THRESHOLD) {
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(LED_YELLOW_PIN, HIGH);
    digitalWrite(LED_GREEN_PIN, LOW);
  } else {
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(LED_YELLOW_PIN, LOW);
    digitalWrite(LED_GREEN_PIN, HIGH);
  }

  Serial.print("Moisture: ");
  Serial.print(moisturePercent);
  Serial.print("%  Temp: ");
  Serial.print(temperature);
  Serial.print("C  Humidity: ");
  Serial.print(humidity);
  Serial.print("%  Days since rain: ");
  Serial.print(daysSinceRain, 2);
  Serial.print("  |  Irrigation probability: ");
  Serial.print(probability * 100, 1);
  Serial.print("%  ->  ");
  Serial.println(shouldIrrigate ? "IRRIGATE" : "OK");

  delay(2000);
}