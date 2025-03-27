#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

const char* ssid = "ketox";  // Replace with your WiFi SSID
const char* password = "";   // Add password if required
const char* weatherApiKey = "66bc6dc00aa6c882b37801e36968757c"; // Your OpenWeatherMap key
const char* geminiApiKey = "AIzaSyDccW7_NuAU8ThIqmzD5-qDICqosIs5NJU"; // Your Gemini key
const char* geminiUrl = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent";

WebServer server(80);

#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

#define SOIL_MOISTURE_PIN 34
#define LED_BUILTIN 2

bool ledState = false;

struct CropData {
  String cropType;
  String location;
  String plantingDate;
};
CropData crops[10];
int cropCount = 0;

void setupSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  loadCropData();
}

void loadCropData() {
  File file = SPIFFS.open("/crops.json", "r");
  if (!file) return;

  DynamicJsonDocument doc(512);
  deserializeJson(doc, file);
  cropCount = doc["count"];
  for (int i = 0; i < cropCount; i++) {
    crops[i].cropType = doc["crops"][i]["type"].as<String>();
    crops[i].location = doc["crops"][i]["location"].as<String>();
    crops[i].plantingDate = doc["crops"][i]["date"].as<String>();
  }
  file.close();
}

void saveCropData() {
  File file = SPIFFS.open("/crops.json", "w");
  if (!file) return;

  DynamicJsonDocument doc(512);
  doc["count"] = cropCount;
  JsonArray cropArray = doc.createNestedArray("crops");
  for (int i = 0; i < cropCount; i++) {
    JsonObject crop = cropArray.createNestedObject();
    crop["type"] = crops[i].cropType;
    crop["location"] = crops[i].location;
    crop["date"] = crops[i].plantingDate;
  }
  serializeJson(doc, file);
  file.close();
}

String getSensorData() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int soilMoisture = analogRead(SOIL_MOISTURE_PIN);

  if (isnan(temperature) || isnan(humidity)) {
    return "{\"error\": \"Sensor reading failed\"}";
  }

  String data = "{";
  data += "\"temperature\": " + String(temperature, 1) + ",";
  data += "\"humidity\": " + String(humidity, 1) + ",";
  data += "\"soil_moisture\": " + String(map(soilMoisture, 0, 4095, 100, 0)); // Inverted: 0 = wet, 4095 = dry
  data += "}";
  return data;
}

String getWeatherData(String location) {
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + location + "&appid=" + String(weatherApiKey) + "&units=metric";
  http.begin(url);
  http.setTimeout(10000);
  int httpCode = http.GET();
  String payload = "{}";
  if (httpCode == HTTP_CODE_OK) {
    payload = http.getString();
  } else {
    Serial.println("Weather API failed, code: " + String(httpCode));
    payload = "{\"error\": \"Weather data unavailable, code: " + String(httpCode) + "\"}";
  }
  http.end();
  return payload;
}

String getCropData(String cropType, String location) {
  String cropData = "{}";
  for (int i = 0; i < cropCount; i++) {
    if (crops[i].cropType == cropType && crops[i].location == location) {
      cropData = "{\"type\": \"" + crops[i].cropType + "\", \"location\": \"" + crops[i].location + "\", \"plantingDate\": \"" + crops[i].plantingDate + "\"}";
      break;
    }
  }
  return cropData;
}

String getAISuggestions(String cropType, String location) {
  HTTPClient http;
  Serial.println("Attempting Gemini API call: " + String(geminiUrl));
  http.begin(String(geminiUrl) + "?key=" + String(geminiApiKey));
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");

  String sensorData = getSensorData();
  String weatherData = getWeatherData(location);
  String cropData = getCropData(cropType, location);

  String prompt = "Given the following data:\n- Sensor Data: " + sensorData + "\n- Weather Data: " + weatherData + "\n- Crop Data: " + cropData + "\nProvide smart irrigation and farming suggestions based on current conditions, historical crop data, and weather forecasts. Include pest management tips if applicable. Format the response under three headings: 'Suggestions', 'Must Do', 'Should You Water the Fields Now (Yes/No)'.";

  DynamicJsonDocument doc(512);
  doc["contents"][0]["parts"][0]["text"] = prompt;

  String requestBody;
  serializeJson(doc, requestBody);
  Serial.println("Request Body: " + requestBody);

  int httpCode = http.POST(requestBody);
  String response;
  if (httpCode == HTTP_CODE_OK) {
    String rawResponse = http.getString();
    Serial.println("Raw Response: " + rawResponse);

    DynamicJsonDocument resDoc(512);
    DeserializationError error = deserializeJson(resDoc, rawResponse);
    if (!error && resDoc.containsKey("candidates") && resDoc["candidates"][0]["content"]["parts"][0]["text"]) {
      String suggestions = resDoc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
      // Escape special characters
      suggestions.replace("\n", "\\n");
      suggestions.replace("\"", "\\\"");
      // Build response as a proper JSON object
      DynamicJsonDocument outDoc(512);
      outDoc["suggestions"] = suggestions;
      outDoc["sensorData"] = sensorData;  // Already a JSON string
      serializeJson(outDoc, response);
    } else {
      response = "{\"error\":\"Invalid Gemini response\",\"sensorData\":" + sensorData + "}";
      Serial.println("Parse Error: " + String(error.c_str()));
    }
  } else {
    response = "{\"error\":\"Gemini API failed, code: " + String(httpCode) + "\",\"sensorData\":" + sensorData + "}";
    Serial.println("HTTP Code: " + String(httpCode));
  }
  Serial.println("Sent Response: " + response);
  http.end();
  return response;
}

String htmlPart1 = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Smart Irrigation</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: Arial, sans-serif; text-align: center; background: #f4f4f4; margin: 0; padding: 0; }
        .container { max-width: 800px; margin: auto; padding: 20px; }
        .card { background: white; padding: 15px; margin: 10px auto; border-radius: 10px; box-shadow: 2px 2px 10px rgba(0, 0, 0, 0.1); }
        button { font-size: 18px; padding: 10px; margin-top: 15px; background: #28a745; color: white; border: none; border-radius: 5px; cursor: pointer; }
        canvas { width: 100% !important; max-height: 200px; }
        #suggestions { margin-top: 20px; white-space: pre-wrap; text-align: left; }
        input { margin: 5px; padding: 5px; }
    </style>
</head>
<body onload="setupCharts()">
    <div class="container">
        <h1>🌿 ESP32 Smart Irrigation 🌿</h1>
        <div class="card"><h2>Temperature</h2><canvas id="tempChart"></canvas></div>
        <div class="card"><h2>Humidity</h2><canvas id="humChart"></canvas></div>
        <div class="card"><h2>Soil Moisture</h2><canvas id="soilChart"></canvas></div>
        <div class="card">
            <h2>Motor Control</h2>
            <button onclick="toggleLED()">Toggle Motor</button>
            <p id="led-status">Motor is OFF</p>
        </div>
        <div class="card">
            <h2>Add Crop</h2>
            <form id="cropForm">
                <input type="text" id="cropType" placeholder="Crop Type" required><br>
                <input type="text" id="location" placeholder="Location" required><br>
                <input type="date" id="plantingDate" required><br>
                <button type="submit">Add Crop</button>
            </form>
        </div>
        <div class="card">
            <h2>AI Suggestions</h2>
            <input type="text" id="aiCrop" placeholder="Crop Type for AI" required>
            <input type="text" id="aiLocation" placeholder="Location for AI" required><br>
            <button onclick="getSuggestions()">Get AI Suggestions</button>
            <p id="suggestions">Click the button to get suggestions</p>
        </div>
    </div>
)rawliteral";

String htmlPart2 = R"rawliteral(
    <script>
        let tempData = [], humData = [], soilData = [], timeLabels = [];
        let chart1, chart2, chart3;

        function updateData() {
            fetch('/data')
            .then(response => response.json())
            .then(data => {
                if (data.error) return;
                let now = new Date().toLocaleTimeString();
                if (timeLabels.length > 10) timeLabels.shift();
                if (tempData.length > 10) tempData.shift();
                if (humData.length > 10) humData.shift();
                if (soilData.length > 10) soilData.shift();
                timeLabels.push(now);
                tempData.push(data.temperature);
                humData.push(data.humidity);
                soilData.push(data.soil_moisture);
                chart1.update();
                chart2.update();
                chart3.update();
            });
        }

        function setupCharts() {
            let ctx1 = document.getElementById('tempChart').getContext('2d');
            let ctx2 = document.getElementById('humChart').getContext('2d');
            let ctx3 = document.getElementById('soilChart').getContext('2d');
            chart1 = new Chart(ctx1, { type: 'line', data: { labels: timeLabels, datasets: [{ label: 'Temperature (°C)', data: tempData, borderColor: 'red', borderWidth: 2 }] }, options: { responsive: true } });
            chart2 = new Chart(ctx2, { type: 'line', data: { labels: timeLabels, datasets: [{ label: 'Humidity (%)', data: humData, borderColor: 'blue', borderWidth: 2 }] }, options: { responsive: true } });
            chart3 = new Chart(ctx3, { type: 'line', data: { labels: timeLabels, datasets: [{ label: 'Soil Moisture (%)', data: soilData, borderColor: 'green', borderWidth: 2 }] }, options: { responsive: true } });
            setInterval(updateData, 5000);
        }

        function toggleLED() {
            fetch('/toggle')
            .then(response => response.text())
            .then(data => document.getElementById('led-status').innerText = data);
        }

        function getSuggestions() {
            let crop = document.getElementById('aiCrop').value;
            let location = document.getElementById('aiLocation').value;
            if (!crop || !location) {
                document.getElementById('suggestions').innerText = 'Please enter crop type and location';
                return;
            }
            document.getElementById('suggestions').innerText = 'Fetching suggestions...';
            fetch(`/suggestions?crop=${encodeURIComponent(crop)}&location=${encodeURIComponent(location)}`)
            .then(response => {
                if (!response.ok) throw new Error('HTTP error ' + response.status);
                return response.text();  // Get raw text first to debug
            })
            .then(text => {
                console.log("Raw response:", text);  // Log raw response in browser console
                const data = JSON.parse(text);  // Parse JSON
                if (data.error) {
                    document.getElementById('suggestions').innerText = 'Error: ' + data.error;
                } else {
                    document.getElementById('suggestions').innerText = data.suggestions.replace(/\\n/g, '\n');
                    let sensorData = JSON.parse(data.sensorData);
                    let now = new Date().toLocaleTimeString();
                    if (timeLabels.length > 10) timeLabels.shift();
                    if (tempData.length > 10) tempData.shift();
                    if (humData.length > 10) humData.shift();
                    if (soilData.length > 10) soilData.shift();
                    timeLabels.push(now);
                    tempData.push(sensorData.temperature);
                    humData.push(sensorData.humidity);
                    soilData.push(sensorData.soil_moisture);
                    chart1.update();
                    chart2.update();
                    chart3.update();
                }
            })
            .catch(error => {
                document.getElementById('suggestions').innerText = 'Error: ' + error.message;
                console.error("Fetch error:", error);
            });
        }

        document.getElementById('cropForm').addEventListener('submit', function(e) {
            e.preventDefault();
            let cropType = document.getElementById('cropType').value;
            let location = document.getElementById('location').value;
            let plantingDate = document.getElementById('plantingDate').value;
            fetch('/addCrop', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ cropType, location, plantingDate })
            }).then(() => {
                alert('Crop added successfully!');
                this.reset();
            });
        });
    </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  String fullPage = htmlPart1 + htmlPart2;
  server.send(200, "text/html", fullPage);
}

void handleData() {
  server.send(200, "application/json", getSensorData());
}

void handleToggleLED() {
  ledState = !ledState;
  digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
  server.send(200, "text/plain", ledState ? "Motor is ON" : "Motor is OFF");
}

void handleAddCrop() {
  if (server.method() == HTTP_POST) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, server.arg("plain"));
    if (cropCount < 10) {
      crops[cropCount].cropType = doc["cropType"].as<String>();
      crops[cropCount].location = doc["location"].as<String>();
      crops[cropCount].plantingDate = doc["plantingDate"].as<String>();
      cropCount++;
      saveCropData();
      server.send(200, "text/plain", "Crop added");
    } else {
      server.send(400, "text/plain", "Crop limit reached");
    }
  }
}

void handleSuggestions() {
  String crop = server.arg("crop");
  String location = server.arg("location");
  if (crop == "" || location == "") {
    server.send(400, "application/json", "{\"error\": \"Crop type and location required\"}");
    return;
  }
  String response = getAISuggestions(crop, location);
  server.send(200, "application/json", response);
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(SOIL_MOISTURE_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  setupSPIFFS();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected! ESP32 IP: " + WiFi.localIP().toString());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/toggle", handleToggleLED);
  server.on("/addCrop", handleAddCrop);
  server.on("/suggestions", HTTP_GET, handleSuggestions);
  server.begin();
}

void loop() {
  server.handleClient();
}