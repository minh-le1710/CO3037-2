#include <Arduino.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino_MQTT_Client.h>
#include <LiquidCrystal_I2C.h>
#include <ThingsBoard.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include "cloud_credentials.h"
#include "DHT20.h"
#include "tinyml_model.h"

using CoreIotClient = ThingsBoardSized<8>;

enum class DisplayState : uint8_t {
  Normal,
  Warning,
  Critical,
};

enum class DeviceTarget : uint8_t {
  StatusLed,
  RgbBeacon,
};

struct SensorSample {
  float temperatureC;
  float humidityPercent;
};

struct SensorReading {
  float temperatureC;
  float humidityPercent;
  float confidence;
  DisplayState state;
};

struct StateChannel {
  SemaphoreHandle_t semaphore;
  QueueHandle_t queue;
  const char *title;
  const char *note;
};

struct DeviceCommand {
  DeviceTarget target;
  bool enabled;
};

struct TinyMlEvaluationSummary {
  float accuracyPercent;
  float averageLatencyUs;
  uint16_t sampleCount;
};

struct CloudTelemetrySummary {
  bool staConfigured;
  bool staConnected;
  bool coreIotConnected;
  uint32_t publishCount;
};

struct DashboardSnapshot {
  SensorReading latestReading;
  bool hasReading;
  bool lcdAvailable;
  bool statusLedOn;
  bool rgbBeaconOn;
  bool apSecured;
  bool apPasswordFallback;
  bool staConfigured;
  bool staConnected;
  bool coreIotConnected;
  bool tinyMlReady;
  uint8_t historyCount;
  uint8_t historyWriteIndex;
  uint16_t tinyMlEvaluationSamples;
  uint32_t coreIotPublishCount;
  char accessPointSsid[33];
  char accessPointPassword[65];
  char accessPointIp[16];
  char stationSsid[33];
  char stationIp[16];
  char coreIotServer[32];
  char tinyMlModelName[32];
  float temperatureHistory[24];
  float humidityHistory[24];
  float tinyMlAccuracyPercent;
  float tinyMlAverageLatencyUs;
};

struct AppContext {
  TwoWire *wire;
  DHT20 sensor;
  LiquidCrystal_I2C *lcd;
  WebServer *server;
  Adafruit_NeoPixel *rgbPixel;
  WiFiClient *cloudWifiClient;
  Arduino_MQTT_Client *cloudMqttClient;
  CoreIotClient *coreIotClient;
  SemaphoreHandle_t lcdMutex;
  SemaphoreHandle_t snapshotMutex;
  QueueHandle_t sensorSampleQueue;
  QueueHandle_t deviceCommandQueue;
  StateChannel normalChannel;
  StateChannel warningChannel;
  StateChannel criticalChannel;
  DashboardSnapshot snapshot;
  int sdaPin;
  int sclPin;
  uint8_t statusLedPin;
  uint8_t rgbLedPin;
};

struct DisplayTaskContext {
  AppContext *app;
  StateChannel *channel;
};

struct TinyMlPrediction {
  DisplayState state;
  float confidencePercent;
};

bool isI2cDevicePresent(TwoWire &wire, uint8_t address) {
  wire.beginTransmission(address);
  return wire.endTransmission() == 0;
}

uint8_t detectLcdAddress(TwoWire &wire) {
  const uint8_t preferredAddresses[] = {0x27, 0x3F, 0x26, 0x3E};

  for (uint8_t address : preferredAddresses) {
    if (isI2cDevicePresent(wire, address)) {
      return address;
    }
  }

  for (uint8_t address = 0x08; address <= 0x77; ++address) {
    if (address == 0x38) {
      continue;
    }

    if (isI2cDevicePresent(wire, address)) {
      return address;
    }
  }

  return 0;
}

DisplayState classifyReading(float temperatureC, float humidityPercent) {
  const bool criticalTemperature = (temperatureC < 20.0f) || (temperatureC >= 32.0f);
  const bool criticalHumidity = (humidityPercent < 30.0f) || (humidityPercent > 80.0f);

  if (criticalTemperature || criticalHumidity) {
    return DisplayState::Critical;
  }

  const bool warningTemperature = (temperatureC < 23.0f) || (temperatureC >= 29.0f);
  const bool warningHumidity = (humidityPercent < 40.0f) || (humidityPercent > 70.0f);

  if (warningTemperature || warningHumidity) {
    return DisplayState::Warning;
  }

  return DisplayState::Normal;
}

DisplayState displayStateFromLabel(uint8_t label) {
  switch (label) {
    case 0:
      return DisplayState::Normal;
    case 1:
      return DisplayState::Warning;
    default:
      return DisplayState::Critical;
  }
}

uint8_t displayStateToLabel(DisplayState state) {
  return static_cast<uint8_t>(state);
}

TinyMlPrediction runTinyMlInference(float temperatureC, float humidityPercent) {
  float normalizedInput[tinyml_model::kInputSize] = {};
  normalizedInput[0] = (temperatureC - tinyml_model::kInputMean[0]) / tinyml_model::kInputStd[0];
  normalizedInput[1] = (humidityPercent - tinyml_model::kInputMean[1]) / tinyml_model::kInputStd[1];

  float hidden1[tinyml_model::kHidden1Size] = {};
  for (size_t j = 0; j < tinyml_model::kHidden1Size; ++j) {
    float value = tinyml_model::kBias1[j];
    for (size_t i = 0; i < tinyml_model::kInputSize; ++i) {
      value += normalizedInput[i] * tinyml_model::kWeights1[i][j];
    }
    hidden1[j] = value > 0.0f ? value : 0.0f;
  }

  float hidden2[tinyml_model::kHidden2Size] = {};
  for (size_t j = 0; j < tinyml_model::kHidden2Size; ++j) {
    float value = tinyml_model::kBias2[j];
    for (size_t i = 0; i < tinyml_model::kHidden1Size; ++i) {
      value += hidden1[i] * tinyml_model::kWeights2[i][j];
    }
    hidden2[j] = value > 0.0f ? value : 0.0f;
  }

  float logits[tinyml_model::kOutputSize] = {};
  float maxLogit = -INFINITY;
  for (size_t j = 0; j < tinyml_model::kOutputSize; ++j) {
    float value = tinyml_model::kBias3[j];
    for (size_t i = 0; i < tinyml_model::kHidden2Size; ++i) {
      value += hidden2[i] * tinyml_model::kWeights3[i][j];
    }
    logits[j] = value;
    if (value > maxLogit) {
      maxLogit = value;
    }
  }

  float probabilitySum = 0.0f;
  float bestProbability = 0.0f;
  uint8_t bestLabel = 0;
  float probabilities[tinyml_model::kOutputSize] = {};

  for (size_t j = 0; j < tinyml_model::kOutputSize; ++j) {
    probabilities[j] = expf(logits[j] - maxLogit);
    probabilitySum += probabilities[j];
  }

  if (probabilitySum <= 0.0f) {
    return {DisplayState::Normal, 0.0f};
  }

  for (size_t j = 0; j < tinyml_model::kOutputSize; ++j) {
    const float probability = probabilities[j] / probabilitySum;
    if (probability >= bestProbability) {
      bestProbability = probability;
      bestLabel = static_cast<uint8_t>(j);
    }
  }

  return {
    displayStateFromLabel(bestLabel),
    bestProbability * 100.0f,
  };
}

const char *displayStateKey(DisplayState state) {
  switch (state) {
    case DisplayState::Normal:
      return "normal";
    case DisplayState::Warning:
      return "warning";
    case DisplayState::Critical:
      return "critical";
  }

  return "normal";
}

const char *displayStateTitle(DisplayState state) {
  switch (state) {
    case DisplayState::Normal:
      return "Bình thường";
    case DisplayState::Warning:
      return "Cảnh báo";
    case DisplayState::Critical:
      return "Nghiêm trọng";
  }

  return "Bình thường";
}

const char *displayStateSummary(DisplayState state) {
  switch (state) {
    case DisplayState::Normal:
      return "Điều kiện đang ổn định";
    case DisplayState::Warning:
      return "Điều kiện cần được theo dõi";
    case DisplayState::Critical:
      return "Điều kiện cần xử lý ngay";
  }

  return "Điều kiện đang ổn định";
}

StateChannel *selectChannel(AppContext *app, DisplayState state) {
  switch (state) {
    case DisplayState::Normal:
      return &app->normalChannel;
    case DisplayState::Warning:
      return &app->warningChannel;
    case DisplayState::Critical:
      return &app->criticalChannel;
  }

  return nullptr;
}

void printPaddedLine(LiquidCrystal_I2C &lcd, uint8_t row, const char *text) {
  char paddedLine[17];
  snprintf(paddedLine, sizeof(paddedLine), "%-16.16s", text);
  lcd.setCursor(0, row);
  lcd.print(paddedLine);
}

void updateSensorSnapshot(AppContext *app, const SensorReading &reading) {
  if (xSemaphoreTake(app->snapshotMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }

  app->snapshot.latestReading = reading;
  app->snapshot.hasReading = true;
  app->snapshot.temperatureHistory[app->snapshot.historyWriteIndex] = reading.temperatureC;
  app->snapshot.humidityHistory[app->snapshot.historyWriteIndex] = reading.humidityPercent;
  app->snapshot.historyWriteIndex = (app->snapshot.historyWriteIndex + 1) % 24;
  if (app->snapshot.historyCount < 24) {
    app->snapshot.historyCount++;
  }

  xSemaphoreGive(app->snapshotMutex);
}

void updateTinyMlSnapshot(AppContext *app, const TinyMlEvaluationSummary &summary) {
  if (xSemaphoreTake(app->snapshotMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }

  app->snapshot.tinyMlReady = true;
  app->snapshot.tinyMlAccuracyPercent = summary.accuracyPercent;
  app->snapshot.tinyMlAverageLatencyUs = summary.averageLatencyUs;
  app->snapshot.tinyMlEvaluationSamples = summary.sampleCount;

  xSemaphoreGive(app->snapshotMutex);
}

void updateCloudSnapshot(AppContext *app, const CloudTelemetrySummary &summary) {
  const String stationIp = summary.staConnected ? WiFi.localIP().toString() : String("0.0.0.0");

  if (xSemaphoreTake(app->snapshotMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }

  app->snapshot.staConfigured = summary.staConfigured;
  app->snapshot.staConnected = summary.staConnected;
  app->snapshot.coreIotConnected = summary.coreIotConnected;
  app->snapshot.coreIotPublishCount = summary.publishCount;
  snprintf(app->snapshot.stationIp, sizeof(app->snapshot.stationIp), "%s", stationIp.c_str());

  xSemaphoreGive(app->snapshotMutex);
}

void updateDeviceSnapshot(AppContext *app, DeviceTarget target, bool enabled) {
  if (xSemaphoreTake(app->snapshotMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }

  if (target == DeviceTarget::StatusLed) {
    app->snapshot.statusLedOn = enabled;
  } else {
    app->snapshot.rgbBeaconOn = enabled;
  }

  xSemaphoreGive(app->snapshotMutex);
}

void updateAccessPointSnapshot(AppContext *app, bool apSecured, bool apPasswordFallback) {
  const String apIp = WiFi.softAPIP().toString();

  if (xSemaphoreTake(app->snapshotMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return;
  }

  app->snapshot.apSecured = apSecured;
  app->snapshot.apPasswordFallback = apPasswordFallback;
  snprintf(app->snapshot.accessPointIp, sizeof(app->snapshot.accessPointIp), "%s", apIp.c_str());

  xSemaphoreGive(app->snapshotMutex);
}

DashboardSnapshot copySnapshot(AppContext *app) {
  DashboardSnapshot snapshot = {};

  if (xSemaphoreTake(app->snapshotMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    snapshot = app->snapshot;
    xSemaphoreGive(app->snapshotMutex);
  }

  return snapshot;
}

bool hasStationCredentials() {
  return strlen(cloud_credentials::kStaSsid) > 0 && strlen(cloud_credentials::kStaPassword) > 0;
}

TinyMlEvaluationSummary evaluateTinyMlOnDevice() {
  uint16_t correctPredictions = 0;
  uint32_t totalLatencyUs = 0;
  uint16_t confusion[3][3] = {};

  for (uint16_t index = 0; index < tinyml_model::kEvaluationSampleCount; ++index) {
    const tinyml_model::EvaluationSample &sample = tinyml_model::kEvaluationSamples[index];

    const uint32_t startTime = micros();
    const TinyMlPrediction prediction = runTinyMlInference(sample.temperatureC, sample.humidityPercent);
    totalLatencyUs += micros() - startTime;

    const uint8_t predictedLabel = displayStateToLabel(prediction.state);
    confusion[sample.label][predictedLabel] += 1;
    if (predictedLabel == sample.label) {
      correctPredictions++;
    }
  }

  Serial.printf(
    "[TinyML] Accuracy: %.2f%% on %u samples\r\n",
    (100.0f * correctPredictions) / tinyml_model::kEvaluationSampleCount,
    tinyml_model::kEvaluationSampleCount
  );
  Serial.printf(
    "[TinyML] Avg inference latency: %.2f us\r\n",
    static_cast<float>(totalLatencyUs) / tinyml_model::kEvaluationSampleCount
  );
  Serial.printf("[TinyML] Confusion matrix:\r\n");
  for (uint8_t row = 0; row < 3; ++row) {
    Serial.printf(
      "[TinyML]   %u | %u %u %u\r\n",
      row,
      confusion[row][0],
      confusion[row][1],
      confusion[row][2]
    );
  }

  return {
    (100.0f * correctPredictions) / tinyml_model::kEvaluationSampleCount,
    static_cast<float>(totalLatencyUs) / tinyml_model::kEvaluationSampleCount,
    tinyml_model::kEvaluationSampleCount,
  };
}

void renderReading(AppContext *app, const SensorReading &reading, const StateChannel &channel) {
  if (app->lcd == nullptr) {
    return;
  }

  if (xSemaphoreTake(app->lcdMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
    return;
  }

  char line1[17];
  char line2[17];

  snprintf(line1, sizeof(line1), "%-8s%4.1fC", channel.title, reading.temperatureC);
  snprintf(line2, sizeof(line2), "H:%4.1f%% M:%3.0f%%", reading.humidityPercent, reading.confidence);

  printPaddedLine(*app->lcd, 0, line1);
  printPaddedLine(*app->lcd, 1, line2);

  xSemaphoreGive(app->lcdMutex);
}

void showAccessPointInfo(AppContext *app) {
  if (app->lcd == nullptr) {
    return;
  }

  const DashboardSnapshot snapshot = copySnapshot(app);

  if (xSemaphoreTake(app->lcdMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
    return;
  }

  printPaddedLine(*app->lcd, 0, "AP CO3037_IOT");
  printPaddedLine(*app->lcd, 1, snapshot.accessPointIp);
  xSemaphoreGive(app->lcdMutex);
}

void setStatusLed(AppContext *app, bool enabled) {
  digitalWrite(app->statusLedPin, enabled ? HIGH : LOW);
}

void setRgbBeacon(AppContext *app, bool enabled) {
  if (app->rgbPixel == nullptr) {
    return;
  }

  const uint32_t accentColor = enabled ? app->rgbPixel->Color(78, 166, 152) : 0;
  app->rgbPixel->setPixelColor(0, accentColor);
  app->rgbPixel->show();
}

bool enqueueDeviceCommand(AppContext *app, DeviceTarget target, bool enabled) {
  DeviceCommand command = {target, enabled};
  return xQueueSend(app->deviceCommandQueue, &command, pdMS_TO_TICKS(150)) == pdPASS;
}

void appendHistoryJson(String &json, const float *values, uint8_t count, uint8_t writeIndex) {
  json += "[";

  if (count > 0) {
    const uint8_t startIndex = count < 24 ? 0 : writeIndex;

    for (uint8_t i = 0; i < count; ++i) {
      const uint8_t index = (startIndex + i) % 24;
      if (i > 0) {
        json += ",";
      }
      json += String(values[index], 1);
    }
  }

  json += "]";
}

String buildStatusJson(const DashboardSnapshot &snapshot) {
  String json;
  json.reserve(1408);

  json += "{";
  json += "\"hasReading\":";
  json += snapshot.hasReading ? "true" : "false";
  json += ",\"temperature\":";
  json += String(snapshot.latestReading.temperatureC, 1);
  json += ",\"humidity\":";
  json += String(snapshot.latestReading.humidityPercent, 1);
  json += ",\"stateKey\":\"";
  json += displayStateKey(snapshot.latestReading.state);
  json += "\"";
  json += ",\"stateTitle\":\"";
  json += displayStateTitle(snapshot.latestReading.state);
  json += "\"";
  json += ",\"stateSummary\":\"";
  json += displayStateSummary(snapshot.latestReading.state);
  json += "\"";
  json += ",\"tinyMlConfidence\":";
  json += String(snapshot.latestReading.confidence, 1);
  json += ",\"tinyMlReady\":";
  json += snapshot.tinyMlReady ? "true" : "false";
  json += ",\"tinyMlAccuracyPercent\":";
  json += String(snapshot.tinyMlAccuracyPercent, 2);
  json += ",\"tinyMlAverageLatencyUs\":";
  json += String(snapshot.tinyMlAverageLatencyUs, 2);
  json += ",\"tinyMlSamples\":";
  json += String(snapshot.tinyMlEvaluationSamples);
  json += ",\"tinyMlModel\":\"";
  json += snapshot.tinyMlModelName;
  json += "\"";
  json += ",\"statusLedOn\":";
  json += snapshot.statusLedOn ? "true" : "false";
  json += ",\"rgbBeaconOn\":";
  json += snapshot.rgbBeaconOn ? "true" : "false";
  json += ",\"lcdAvailable\":";
  json += snapshot.lcdAvailable ? "true" : "false";
  json += ",\"ssid\":\"";
  json += snapshot.accessPointSsid;
  json += "\"";
  json += ",\"requestedPassword\":\"";
  json += snapshot.accessPointPassword;
  json += "\"";
  json += ",\"accessPointIp\":\"";
  json += snapshot.accessPointIp;
  json += "\"";
  json += ",\"stationSsid\":\"";
  json += snapshot.stationSsid;
  json += "\"";
  json += ",\"stationIp\":\"";
  json += snapshot.stationIp;
  json += "\"";
  json += ",\"coreIotServer\":\"";
  json += snapshot.coreIotServer;
  json += "\"";
  json += ",\"apSecured\":";
  json += snapshot.apSecured ? "true" : "false";
  json += ",\"apPasswordFallback\":";
  json += snapshot.apPasswordFallback ? "true" : "false";
  json += ",\"staConfigured\":";
  json += snapshot.staConfigured ? "true" : "false";
  json += ",\"staConnected\":";
  json += snapshot.staConnected ? "true" : "false";
  json += ",\"coreIotConnected\":";
  json += snapshot.coreIotConnected ? "true" : "false";
  json += ",\"coreIotPublishCount\":";
  json += String(snapshot.coreIotPublishCount);
  json += ",\"temperatureHistory\":";
  appendHistoryJson(
    json,
    snapshot.temperatureHistory,
    snapshot.historyCount,
    snapshot.historyWriteIndex
  );
  json += ",\"humidityHistory\":";
  appendHistoryJson(
    json,
    snapshot.humidityHistory,
    snapshot.historyCount,
    snapshot.historyWriteIndex
  );
  json += "}";

  return json;
}

String buildDashboardPage() {
  return R"rawliteral(
<!doctype html>
<html lang="vi">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <title>CO3037</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Be+Vietnam+Pro:wght@400;500;600;700;800&display=swap" rel="stylesheet">
  <style>
    :root {
      color-scheme: light;
      --bg: #f6f8fb;
      --panel: rgba(255, 255, 255, 0.9);
      --panel-strong: #ffffff;
      --stroke: #dbe3ea;
      --stroke-soft: #edf1f5;
      --text: #13202d;
      --muted: #607182;
      --accent: #1f4e79;
      --red: #d94b4b;
      --green: #159957;
      --warning: #d4a453;
      --shadow: 0 18px 50px rgba(15, 30, 46, 0.08);
      --radius: 8px;
    }

    * {
      box-sizing: border-box;
    }

    body {
      margin: 0;
      min-height: 100vh;
      font-family: "Be Vietnam Pro", "Segoe UI", system-ui, sans-serif;
      color: var(--text);
      background: var(--bg);
      -webkit-text-size-adjust: 100%;
    }

    .shell {
      position: relative;
      min-height: 100vh;
      padding-top: max(28px, env(safe-area-inset-top));
      padding-right: max(18px, env(safe-area-inset-right));
      padding-bottom: max(28px, env(safe-area-inset-bottom));
      padding-left: max(18px, env(safe-area-inset-left));
      display: flex;
      justify-content: center;
    }

    .shell::before {
      content: "";
      position: fixed;
      inset: 0;
      background:
        linear-gradient(90deg, rgba(255, 255, 255, 0.96), rgba(255, 255, 255, 0.88)),
        url('https://images.pexels.com/photos/1103970/pexels-photo-1103970.jpeg');
      background-size: cover;
      background-position: center;
      z-index: -1;
    }

    .console {
      width: min(1180px, 100%);
      display: grid;
      gap: 20px;
      padding: 26px;
      border-radius: var(--radius);
      border: 1px solid rgba(219, 227, 234, 0.78);
      background: linear-gradient(180deg, rgba(255, 255, 255, 0.9), rgba(255, 255, 255, 0.82));
      backdrop-filter: blur(12px);
      box-shadow: var(--shadow);
    }

    .topbar {
      display: grid;
      gap: 16px;
      grid-template-columns: auto 1fr;
      align-items: center;
    }

    h1 {
      margin: 0;
      font-size: clamp(32px, 5vw, 44px);
      line-height: 1;
      letter-spacing: 0;
    }

    .network-band {
      display: grid;
      gap: 12px;
      grid-template-columns: repeat(4, minmax(0, 1fr));
    }

    .network-item,
    .metric-card,
    .device-card,
    .chart-card {
      min-width: 0;
      border: 1px solid var(--stroke);
      border-radius: var(--radius);
      background: rgba(255, 255, 255, 0.84);
    }

    .network-item {
      padding: 14px 16px;
    }

    .network-label,
    .metric-label,
    .device-label,
    .section-label {
      display: block;
      font-size: 12px;
      color: var(--muted);
      text-transform: uppercase;
    }

    .network-value {
      display: block;
      margin-top: 8px;
      font-size: clamp(15px, 2vw, 17px);
      font-weight: 700;
      line-height: 1.35;
      overflow-wrap: anywhere;
    }

    .layout {
      display: grid;
      gap: 20px;
      grid-template-columns: minmax(0, 1.35fr) minmax(320px, 0.8fr);
    }

    .chart-card {
      padding: 20px;
      display: grid;
      gap: 18px;
      overflow: hidden;
    }

    .card-head {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      flex-wrap: wrap;
    }

    .legend {
      display: flex;
      gap: 16px;
      flex-wrap: wrap;
      color: var(--muted);
      font-size: 13px;
      font-weight: 600;
    }

    .legend-item {
      display: inline-flex;
      align-items: center;
      gap: 8px;
    }

    .legend-dot {
      width: 10px;
      height: 10px;
      border-radius: 999px;
      flex: 0 0 auto;
    }

    .legend-dot.red {
      background: var(--red);
    }

    .legend-dot.green {
      background: var(--green);
    }

    .metrics {
      display: grid;
      gap: 12px;
      grid-template-columns: repeat(3, minmax(0, 1fr));
    }

    .metric-card {
      padding: 16px;
    }

    .metric-value {
      margin-top: 10px;
      display: flex;
      align-items: baseline;
      gap: 6px;
      font-size: clamp(26px, 4vw, 40px);
      font-weight: 800;
      line-height: 1;
    }

    .metric-unit {
      font-size: 13px;
      color: var(--muted);
      text-transform: uppercase;
    }

    .state-pill {
      margin-top: 12px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      min-height: 38px;
      padding: 8px 14px;
      border-radius: 999px;
      border: 1px solid rgba(31, 78, 121, 0.14);
      background: rgba(31, 78, 121, 0.08);
      color: var(--accent);
      font-size: 13px;
      font-weight: 700;
    }

    .state-pill.warning {
      color: #8a6213;
      border-color: rgba(212, 164, 83, 0.22);
      background: rgba(212, 164, 83, 0.12);
    }

    .state-pill.critical {
      color: #ae3131;
      border-color: rgba(217, 75, 75, 0.22);
      background: rgba(217, 75, 75, 0.12);
    }

    .chart-shell {
      border: 1px solid var(--stroke-soft);
      border-radius: var(--radius);
      background: linear-gradient(180deg, #ffffff, #fbfcfd);
      padding: 10px;
    }

    .chart-svg {
      display: block;
      width: 100%;
      height: 280px;
    }

    .side {
      display: grid;
      gap: 16px;
      align-content: start;
    }

    .device-card {
      padding: 18px;
      display: grid;
      gap: 14px;
    }

    .device-top {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
    }

    .device-title {
      margin: 4px 0 0;
      font-size: 20px;
      font-weight: 700;
    }

    .device-state {
      font-size: 13px;
      font-weight: 700;
      color: var(--muted);
      text-transform: uppercase;
    }

    .device-state.on {
      color: var(--accent);
    }

    .controls {
      display: grid;
      gap: 10px;
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }

    button {
      width: 100%;
      min-height: 48px;
      border-radius: var(--radius);
      border: 1px solid var(--stroke);
      background: #ffffff;
      color: var(--text);
      font: inherit;
      font-size: 14px;
      font-weight: 700;
      cursor: pointer;
      touch-action: manipulation;
      -webkit-tap-highlight-color: transparent;
      transition: transform 150ms ease, box-shadow 150ms ease, border-color 150ms ease;
    }

    button:hover {
      transform: translateY(-1px);
      box-shadow: 0 10px 24px rgba(15, 30, 46, 0.08);
      border-color: #c7d3dd;
    }

    button.primary {
      background: #f6fbff;
      color: var(--accent);
      border-color: #cfe1f4;
    }

    button.secondary {
      background: #ffffff;
      color: #6a7a88;
    }

    .footer-note {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      color: var(--muted);
      font-size: 13px;
    }

    @media (max-width: 980px) {
      .topbar,
      .layout {
        grid-template-columns: 1fr;
      }

      .network-band,
      .metrics {
        grid-template-columns: repeat(2, minmax(0, 1fr));
      }

      .metric-card.state-card {
        grid-column: 1 / -1;
      }

      .console {
        padding: 20px;
      }

      .chart-svg {
        height: 248px;
      }
    }

    @media (max-width: 640px) {
      .shell {
        padding-top: max(14px, env(safe-area-inset-top));
        padding-right: max(12px, env(safe-area-inset-right));
        padding-bottom: max(14px, env(safe-area-inset-bottom));
        padding-left: max(12px, env(safe-area-inset-left));
      }

      .console {
        gap: 16px;
        padding: 16px;
      }

      .topbar {
        gap: 12px;
      }

      h1 {
        font-size: 30px;
      }

      .network-band {
        gap: 10px;
      }

      .network-item,
      .metric-card {
        padding: 14px;
      }

      .chart-card,
      .device-card {
        padding: 16px;
      }

      .chart-card {
        gap: 16px;
      }

      .chart-shell {
        padding: 8px;
      }

      .chart-svg {
        height: 220px;
      }

      .device-top,
      .footer-note {
        flex-direction: column;
        align-items: flex-start;
      }

      .device-top {
        gap: 8px;
      }
    }

    @media (max-width: 420px) {
      .network-band,
      .metrics,
      .controls {
        grid-template-columns: 1fr;
      }

      .metric-card.state-card {
        grid-column: auto;
      }

      .chart-svg {
        height: 208px;
      }

      .legend {
        gap: 10px;
      }
    }
  </style>
</head>
<body>
  <main class="shell">
    <section class="console">
      <header class="topbar">
        <h1>CO3037</h1>
        <div class="network-band">
          <div class="network-item">
            <span class="network-label">Access Point</span>
            <strong class="network-value" id="ssidValue">CO3037_IOT</strong>
          </div>
          <div class="network-item">
            <span class="network-label">IP truy cập AP</span>
            <strong class="network-value" id="ipValue">192.168.4.1</strong>
          </div>
          <div class="network-item">
            <span class="network-label">Station WiFi</span>
            <strong class="network-value" id="stationValue">Chưa cấu hình</strong>
          </div>
          <div class="network-item">
            <span class="network-label">CoreIOT</span>
            <strong class="network-value" id="cloudValue">Đang khởi tạo</strong>
          </div>
        </div>
      </header>

      <section class="layout">
        <article class="chart-card">
          <div class="card-head">
            <span class="section-label">Biểu đồ thời gian thực</span>
            <div class="legend">
              <span class="legend-item"><span class="legend-dot red"></span>Nhiệt độ</span>
              <span class="legend-item"><span class="legend-dot green"></span>Độ ẩm</span>
            </div>
          </div>

          <div class="metrics">
            <article class="metric-card">
              <span class="metric-label">Nhiệt độ</span>
              <div class="metric-value"><span id="temperatureValue">--</span><span class="metric-unit">°C</span></div>
            </article>
            <article class="metric-card">
              <span class="metric-label">Độ ẩm</span>
              <div class="metric-value"><span id="humidityValue">--</span><span class="metric-unit">%</span></div>
            </article>
            <article class="metric-card state-card">
              <span class="metric-label">Trạng thái TinyML</span>
              <div class="state-pill" id="statePill">Đang chờ</div>
            </article>
          </div>

          <div class="chart-shell">
            <svg id="historyChart" class="chart-svg" viewBox="0 0 800 280" preserveAspectRatio="none"></svg>
          </div>
        </article>

        <aside class="side">
          <article class="device-card">
            <div class="device-top">
              <div>
                <span class="device-label">Thiết bị 1</span>
                <h3 class="device-title">Đèn trạng thái</h3>
              </div>
              <span class="device-state" id="statusLedState">Tắt</span>
            </div>
            <div class="controls">
              <button class="primary" type="button" onclick="setDevice('status-led','on')">Bật</button>
              <button class="secondary" type="button" onclick="setDevice('status-led','off')">Tắt</button>
            </div>
          </article>

          <article class="device-card">
            <div class="device-top">
              <div>
                <span class="device-label">Thiết bị 2</span>
                <h3 class="device-title">Đèn RGB</h3>
              </div>
              <span class="device-state" id="rgbBeaconState">Tắt</span>
            </div>
            <div class="controls">
              <button class="primary" type="button" onclick="setDevice('rgb-beacon','on')">Bật</button>
              <button class="secondary" type="button" onclick="setDevice('rgb-beacon','off')">Tắt</button>
            </div>
          </article>
        </aside>
      </section>

      <footer class="footer-note">
        <span id="lcdStatus">LCD: Đang chờ</span>
        <span id="mlStatus">TinyML: Đang đánh giá</span>
        <span id="cloudStatus">CoreIOT: Đang chờ</span>
        <span id="refreshState">Cập nhật thời gian thực</span>
      </footer>
    </section>
  </main>

  <script>
    function linePoints(values, minValue, maxValue, left, top, width, height) {
      if (!values.length) {
        return '';
      }

      const safeRange = Math.max(maxValue - minValue, 0.1);
      return values.map((value, index) => {
        const x = values.length === 1
          ? left + width / 2
          : left + (width * index) / (values.length - 1);
        const y = top + height - ((value - minValue) / safeRange) * height;
        return x.toFixed(1) + ',' + y.toFixed(1);
      }).join(' ');
    }

    function dotMarkup(values, minValue, maxValue, left, top, width, height, color, radius) {
      if (!values.length) {
        return '';
      }

      const safeRange = Math.max(maxValue - minValue, 0.1);
      return values.map((value, index) => {
        const x = values.length === 1
          ? left + width / 2
          : left + (width * index) / (values.length - 1);
        const y = top + height - ((value - minValue) / safeRange) * height;
        return '<circle cx="' + x.toFixed(1) + '" cy="' + y.toFixed(1) + '" r="' + radius + '" fill="' + color + '" />';
      }).join('');
    }

    function renderChart(tempHistory, humidityHistory) {
      const svg = document.getElementById('historyChart');
      const compact = window.matchMedia('(max-width: 640px)').matches;
      const canvasWidth = 800;
      const canvasHeight = compact ? 220 : 280;
      const left = compact ? 44 : 58;
      const right = compact ? 44 : 58;
      const top = compact ? 18 : 22;
      const bottom = compact ? 28 : 34;
      const width = canvasWidth - left - right;
      const height = canvasHeight - top - bottom;
      const labelFontSize = compact ? 11 : 12;
      const dotRadius = compact ? 3.8 : 4.2;
      const tempSeries = Array.isArray(tempHistory) ? tempHistory : [];
      const humiditySeries = Array.isArray(humidityHistory) ? humidityHistory : [];
      const pointCount = Math.max(tempSeries.length, humiditySeries.length);

      svg.setAttribute('viewBox', '0 0 ' + canvasWidth + ' ' + canvasHeight);

      if (!pointCount) {
        svg.innerHTML = '<rect x="0" y="0" width="' + canvasWidth + '" height="' + canvasHeight + '" rx="8" fill="#ffffff"></rect><text x="' + (canvasWidth / 2) + '" y="' + ((canvasHeight / 2) + 6) + '" text-anchor="middle" font-size="' + (compact ? 14 : 16) + '" fill="#8a96a3" font-family="Be Vietnam Pro, sans-serif">Đang chờ dữ liệu cảm biến</text>';
        return;
      }

      const tempMinRaw = Math.min(...tempSeries);
      const tempMaxRaw = Math.max(...tempSeries);
      const tempPadding = Math.max((tempMaxRaw - tempMinRaw) * 0.18, 1.2);
      const tempMin = tempMinRaw - tempPadding;
      const tempMax = tempMaxRaw + tempPadding;
      const humidityMin = 0;
      const humidityMax = 100;

      let grid = '';
      for (let i = 0; i < 5; i += 1) {
        const y = top + (height * i) / 4;
        const tempLabel = (tempMax - ((tempMax - tempMin) * i) / 4).toFixed(1);
        const humidityLabel = (humidityMax - ((humidityMax - humidityMin) * i) / 4).toFixed(0);

        grid += '<line x1="' + left + '" y1="' + y.toFixed(1) + '" x2="' + (left + width) + '" y2="' + y.toFixed(1) + '" stroke="#e8edf2" stroke-width="1" />';
        grid += '<text x="' + (compact ? 12 : 18) + '" y="' + (y + 4).toFixed(1) + '" font-size="' + labelFontSize + '" fill="#788796" font-family="Be Vietnam Pro, sans-serif">' + tempLabel + '°</text>';
        grid += '<text x="' + (canvasWidth - (compact ? 12 : 20)) + '" y="' + (y + 4).toFixed(1) + '" text-anchor="end" font-size="' + labelFontSize + '" fill="#788796" font-family="Be Vietnam Pro, sans-serif">' + humidityLabel + '%</text>';
      }

      let verticals = '';
      const verticalCount = Math.min(pointCount, compact ? 4 : 6);
      for (let i = 0; i < verticalCount; i += 1) {
        const x = verticalCount === 1
          ? left + width / 2
          : left + (width * i) / (verticalCount - 1);
        verticals += '<line x1="' + x.toFixed(1) + '" y1="' + top + '" x2="' + x.toFixed(1) + '" y2="' + (top + height) + '" stroke="#f1f4f7" stroke-width="1" />';
      }

      const tempPolyline = linePoints(tempSeries, tempMin, tempMax, left, top, width, height);
      const humidityPolyline = linePoints(humiditySeries, humidityMin, humidityMax, left, top, width, height);
      const tempDots = dotMarkup(tempSeries, tempMin, tempMax, left, top, width, height, '#d94b4b', dotRadius);
      const humidityDots = dotMarkup(humiditySeries, humidityMin, humidityMax, left, top, width, height, '#159957', dotRadius);

      svg.innerHTML =
        '<rect x="0" y="0" width="' + canvasWidth + '" height="' + canvasHeight + '" rx="8" fill="#ffffff"></rect>' +
        grid +
        verticals +
        '<line x1="' + left + '" y1="' + (top + height) + '" x2="' + (left + width) + '" y2="' + (top + height) + '" stroke="#d7e0e8" stroke-width="1.2" />' +
        '<line x1="' + left + '" y1="' + top + '" x2="' + left + '" y2="' + (top + height) + '" stroke="#d7e0e8" stroke-width="1.2" />' +
        '<polyline fill="none" stroke="#d94b4b" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round" points="' + tempPolyline + '"></polyline>' +
        '<polyline fill="none" stroke="#159957" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round" points="' + humidityPolyline + '"></polyline>' +
        tempDots +
        humidityDots;
    }

    async function refreshStatus() {
      try {
        const response = await fetch('/status', { cache: 'no-store' });
        const data = await response.json();

        document.getElementById('ssidValue').textContent = data.ssid;
        document.getElementById('ipValue').textContent = data.accessPointIp;
        document.getElementById('stationValue').textContent = data.staConfigured
          ? (data.staConnected ? data.stationSsid + ' | ' + data.stationIp : data.stationSsid + ' | Đang kết nối')
          : 'Chưa cấu hình';
        document.getElementById('cloudValue').textContent = data.coreIotConnected
          ? 'Đã kết nối'
          : (data.staConfigured ? 'Đang chờ MQTT' : 'Thiếu WiFi STA');
        document.getElementById('lcdStatus').textContent = data.lcdAvailable ? 'LCD: Sẵn sàng' : 'LCD: Không kết nối';
        document.getElementById('mlStatus').textContent = data.tinyMlReady
          ? 'TinyML: ' + data.tinyMlAccuracyPercent.toFixed(2) + '% / ' + data.tinyMlSamples + ' mẫu | ' + data.tinyMlAverageLatencyUs.toFixed(1) + ' us | Tin cậy ' + data.tinyMlConfidence.toFixed(1) + '%'
          : 'TinyML: Đang đánh giá';
        document.getElementById('cloudStatus').textContent = data.coreIotConnected
          ? 'CoreIOT: ' + data.coreIotServer + ' | ' + data.coreIotPublishCount + ' lần gửi'
          : (data.staConfigured ? 'CoreIOT: Chưa kết nối server' : 'CoreIOT: Cần điền WiFi STA');

        const statePill = document.getElementById('statePill');
        statePill.textContent = data.stateTitle;
        statePill.className = 'state-pill ' + data.stateKey;

        document.getElementById('temperatureValue').textContent = data.hasReading ? data.temperature.toFixed(1) : '--';
        document.getElementById('humidityValue').textContent = data.hasReading ? data.humidity.toFixed(1) : '--';

        const statusLedState = document.getElementById('statusLedState');
        statusLedState.textContent = data.statusLedOn ? 'Bật' : 'Tắt';
        statusLedState.className = 'device-state ' + (data.statusLedOn ? 'on' : '');

        const rgbBeaconState = document.getElementById('rgbBeaconState');
        rgbBeaconState.textContent = data.rgbBeaconOn ? 'Bật' : 'Tắt';
        rgbBeaconState.className = 'device-state ' + (data.rgbBeaconOn ? 'on' : '');

        renderChart(data.temperatureHistory, data.humidityHistory);
        document.getElementById('refreshState').textContent = data.apPasswordFallback
          ? 'Mật khẩu dưới 8 ký tự nên AP đang mở'
          : (data.staConnected ? 'AP + STA đang hoạt động' : 'Cập nhật mỗi 2 giây');
      } catch (error) {
        document.getElementById('refreshState').textContent = 'Đang thử kết nối lại';
      }
    }

    async function setDevice(target, action) {
      const body = new URLSearchParams({ target, action });

      try {
        document.getElementById('refreshState').textContent = 'Đang gửi lệnh';
        await fetch('/api/device', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
          body: body.toString()
        });
      } finally {
        setTimeout(refreshStatus, 150);
      }
    }

    refreshStatus();
    setInterval(refreshStatus, 2000);
  </script>
</body>
</html>
)rawliteral";
}

void handleStatusRequest(AppContext *app) {
  const DashboardSnapshot snapshot = copySnapshot(app);
  app->server->send(200, "application/json; charset=utf-8", buildStatusJson(snapshot));
}

void handleDeviceCommandRequest(AppContext *app) {
  const String target = app->server->arg("target");
  const String action = app->server->arg("action");
  const bool turnOn = action == "on";
  const bool validAction = action == "on" || action == "off";

  if (!validAction) {
    app->server->send(400, "application/json; charset=utf-8", "{\"ok\":false,\"message\":\"Invalid action\"}");
    return;
  }

  bool queued = false;
  if (target == "status-led") {
    queued = enqueueDeviceCommand(app, DeviceTarget::StatusLed, turnOn);
  } else if (target == "rgb-beacon") {
    queued = enqueueDeviceCommand(app, DeviceTarget::RgbBeacon, turnOn);
  } else {
    app->server->send(400, "application/json; charset=utf-8", "{\"ok\":false,\"message\":\"Invalid target\"}");
    return;
  }

  if (!queued) {
    app->server->send(503, "application/json; charset=utf-8", "{\"ok\":false,\"message\":\"Device queue busy\"}");
    return;
  }

  app->server->send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void configureWebServer(AppContext *app) {
  app->server = new WebServer(80);

  app->server->on("/", HTTP_GET, [app]() {
    app->server->send(200, "text/html; charset=utf-8", buildDashboardPage());
  });

  app->server->on("/status", HTTP_GET, [app]() {
    handleStatusRequest(app);
  });

  app->server->on("/api/device", HTTP_POST, [app]() {
    handleDeviceCommandRequest(app);
  });

  app->server->onNotFound([app]() {
    app->server->send(404, "text/plain; charset=utf-8", "Not Found");
  });

  app->server->begin();
}

bool initializeLcd(AppContext *app) {
  const uint8_t lcdAddress = detectLcdAddress(*app->wire);
  if (lcdAddress == 0) {
    Serial.println("[LCD] No supported I2C LCD found");
    return false;
  }

  app->lcd = new LiquidCrystal_I2C(lcdAddress, 16, 2);
  app->lcd->init();
  app->lcd->backlight();

  printPaddedLine(*app->lcd, 0, "Task 6 CoreIOT");
  printPaddedLine(*app->lcd, 1, "Starting AP");

  Serial.printf("[LCD] Connected at 0x%02X\r\n", lcdAddress);
  return true;
}

bool initializeChannels(AppContext *app) {
  app->lcdMutex = xSemaphoreCreateMutex();
  app->snapshotMutex = xSemaphoreCreateMutex();
  app->sensorSampleQueue = xQueueCreate(1, sizeof(SensorSample));
  app->deviceCommandQueue = xQueueCreate(6, sizeof(DeviceCommand));
  app->normalChannel = {xSemaphoreCreateBinary(), xQueueCreate(1, sizeof(SensorReading)), "NORMAL", "OK"};
  app->warningChannel = {xSemaphoreCreateBinary(), xQueueCreate(1, sizeof(SensorReading)), "WARNING", "CHECK"};
  app->criticalChannel = {xSemaphoreCreateBinary(), xQueueCreate(1, sizeof(SensorReading)), "CRITICAL", "ALERT"};

  return app->lcdMutex != nullptr &&
         app->snapshotMutex != nullptr &&
         app->sensorSampleQueue != nullptr &&
         app->deviceCommandQueue != nullptr &&
         app->normalChannel.semaphore != nullptr &&
         app->warningChannel.semaphore != nullptr &&
         app->criticalChannel.semaphore != nullptr &&
         app->normalChannel.queue != nullptr &&
         app->warningChannel.queue != nullptr &&
         app->criticalChannel.queue != nullptr;
}

void initializeSnapshot(AppContext *app) {
  memset(&app->snapshot, 0, sizeof(app->snapshot));
  app->snapshot.latestReading.state = DisplayState::Normal;
  app->snapshot.latestReading.confidence = 0.0f;
  app->snapshot.lcdAvailable = false;
  app->snapshot.tinyMlReady = false;
  app->snapshot.staConfigured = hasStationCredentials();
  snprintf(app->snapshot.stationSsid, sizeof(app->snapshot.stationSsid), "%s", cloud_credentials::kStaSsid);
  snprintf(app->snapshot.stationIp, sizeof(app->snapshot.stationIp), "%s", "0.0.0.0");
  snprintf(app->snapshot.coreIotServer, sizeof(app->snapshot.coreIotServer), "%s", cloud_credentials::kCoreIotServer);
  snprintf(app->snapshot.accessPointSsid, sizeof(app->snapshot.accessPointSsid), "%s", "CO3037_IOT");
  snprintf(app->snapshot.accessPointPassword, sizeof(app->snapshot.accessPointPassword), "%s", "123456");
  snprintf(app->snapshot.accessPointIp, sizeof(app->snapshot.accessPointIp), "%s", "0.0.0.0");
  snprintf(app->snapshot.tinyMlModelName, sizeof(app->snapshot.tinyMlModelName), "%s", tinyml_model::kModelName);
}

void initializeRgbBeacon(AppContext *app) {
  app->rgbPixel = new Adafruit_NeoPixel(1, app->rgbLedPin, NEO_GRB + NEO_KHZ800);
  app->rgbPixel->begin();
  app->rgbPixel->setBrightness(24);
  app->rgbPixel->clear();
  app->rgbPixel->show();
}

void startAccessPoint(AppContext *app) {
  const size_t passwordLength = strlen(app->snapshot.accessPointPassword);
  const bool passwordValid = passwordLength >= 8;
  const bool accessPointReady = passwordValid
    ? WiFi.softAP(app->snapshot.accessPointSsid, app->snapshot.accessPointPassword)
    : WiFi.softAP(app->snapshot.accessPointSsid);

  if (!accessPointReady) {
    Serial.println("[WiFi] Failed to start access point");
    return;
  }

  if (!passwordValid) {
    Serial.println("[WiFi] Requested AP password is shorter than 8 characters; starting open AP fallback");
  }

  updateAccessPointSnapshot(app, passwordValid, !passwordValid);

  Serial.print("[WiFi] SSID: ");
  Serial.println(app->snapshot.accessPointSsid);
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.softAPIP());
}

void initializeCoreIot(AppContext *app) {
  app->cloudWifiClient = new WiFiClient();
  app->cloudMqttClient = new Arduino_MQTT_Client(*app->cloudWifiClient);
  app->coreIotClient = new CoreIotClient(*app->cloudMqttClient, 256U, 256U);
}

bool ensureStationConnected(uint32_t &lastAttemptMs) {
  if (!hasStationCredentials()) {
    return false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  const uint32_t now = millis();
  if (now - lastAttemptMs < cloud_credentials::kReconnectDelayMs) {
    return false;
  }

  lastAttemptMs = now;
  Serial.printf("[WiFi] Connecting STA to %s\r\n", cloud_credentials::kStaSsid);
  WiFi.begin(cloud_credentials::kStaSsid, cloud_credentials::kStaPassword);
  return false;
}

bool ensureCoreIotConnected(AppContext *app, bool &attributesSent) {
  if (app->coreIotClient == nullptr) {
    return false;
  }

  if (app->coreIotClient->connected()) {
    return true;
  }

  attributesSent = false;
  Serial.printf("[CoreIOT] Connecting to %s\r\n", cloud_credentials::kCoreIotServer);
  if (!app->coreIotClient->connect(
        cloud_credentials::kCoreIotServer,
        cloud_credentials::kCoreIotToken,
        cloud_credentials::kCoreIotPort
      )) {
    Serial.println("[CoreIOT] MQTT connect failed");
    return false;
  }

  Serial.println("[CoreIOT] MQTT connected");
  return true;
}

void publishCoreIotAttributes(AppContext *app) {
  if (app->coreIotClient == nullptr) {
    return;
  }

  app->coreIotClient->sendAttributeData("mac_address", WiFi.macAddress().c_str());
  app->coreIotClient->sendAttributeData("ip_address", WiFi.localIP().toString().c_str());
  app->coreIotClient->sendAttributeData("ssid", WiFi.SSID().c_str());
  app->coreIotClient->sendAttributeData("bssid", WiFi.BSSIDstr().c_str());
  app->coreIotClient->sendAttributeData("channel", WiFi.channel());
  app->coreIotClient->sendAttributeData("tinyml_model", tinyml_model::kModelName);
}

void publishCoreIotTelemetry(AppContext *app, const DashboardSnapshot &snapshot) {
  if (app->coreIotClient == nullptr || !snapshot.hasReading) {
    return;
  }

  app->coreIotClient->sendTelemetryData("temperature", snapshot.latestReading.temperatureC);
  app->coreIotClient->sendTelemetryData("humidity", snapshot.latestReading.humidityPercent);
  app->coreIotClient->sendTelemetryData("tinyml_confidence", snapshot.latestReading.confidence);
  app->coreIotClient->sendTelemetryData("tinyml_state", displayStateKey(snapshot.latestReading.state));
  app->coreIotClient->sendTelemetryData("tinyml_accuracy", snapshot.tinyMlAccuracyPercent);
  app->coreIotClient->sendTelemetryData("rssi", WiFi.RSSI());
}

void sensorTask(void *pvParameters) {
  auto *app = static_cast<AppContext *>(pvParameters);
  constexpr TickType_t samplePeriod = pdMS_TO_TICKS(2000);

  for (;;) {
    app->sensor.read();

    const float temperatureC = app->sensor.getTemperature();
    const float humidityPercent = app->sensor.getHumidity();

    if (isnan(temperatureC) || isnan(humidityPercent)) {
      Serial.println("[Sensor] Failed to read DHT20 data");
      vTaskDelay(samplePeriod);
      continue;
    }

    SensorSample sample = {
      temperatureC,
      humidityPercent,
    };

    if (xQueueOverwrite(app->sensorSampleQueue, &sample) != pdPASS) {
      Serial.println("[Sensor] Failed to publish sample to TinyML queue");
    }

    Serial.printf("[Sensor] T=%.1f C, H=%.1f %%\r\n", temperatureC, humidityPercent);
    vTaskDelay(samplePeriod);
  }
}

void inferenceTask(void *pvParameters) {
  auto *app = static_cast<AppContext *>(pvParameters);
  SensorSample sample = {};

  for (;;) {
    if (xQueueReceive(app->sensorSampleQueue, &sample, portMAX_DELAY) != pdPASS) {
      continue;
    }

    const TinyMlPrediction prediction = runTinyMlInference(sample.temperatureC, sample.humidityPercent);
    SensorReading reading = {
      sample.temperatureC,
      sample.humidityPercent,
      prediction.confidencePercent,
      prediction.state,
    };

    updateSensorSnapshot(app, reading);

    StateChannel *targetChannel = selectChannel(app, reading.state);
    if (targetChannel != nullptr) {
      xQueueOverwrite(targetChannel->queue, &reading);
      xSemaphoreGive(targetChannel->semaphore);
    }

    Serial.printf(
      "[TinyML] %s (%.1f%%) for T=%.1f C, H=%.1f %%\r\n",
      displayStateTitle(reading.state),
      reading.confidence,
      reading.temperatureC,
      reading.humidityPercent
    );
  }
}

void displayTask(void *pvParameters) {
  auto *taskContext = static_cast<DisplayTaskContext *>(pvParameters);
  SensorReading reading = {};

  for (;;) {
    if (xSemaphoreTake(taskContext->channel->semaphore, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (xQueueReceive(taskContext->channel->queue, &reading, 0) != pdPASS) {
      continue;
    }

    renderReading(taskContext->app, reading, *taskContext->channel);
    Serial.printf(
      "[Display:%s] T=%.1f C, H=%.1f %%\r\n",
      taskContext->channel->title,
      reading.temperatureC,
      reading.humidityPercent
    );
  }
}

void deviceTask(void *pvParameters) {
  auto *app = static_cast<AppContext *>(pvParameters);
  DeviceCommand command = {};

  pinMode(app->statusLedPin, OUTPUT);
  setStatusLed(app, false);
  setRgbBeacon(app, false);
  updateDeviceSnapshot(app, DeviceTarget::StatusLed, false);
  updateDeviceSnapshot(app, DeviceTarget::RgbBeacon, false);

  for (;;) {
    if (xQueueReceive(app->deviceCommandQueue, &command, portMAX_DELAY) != pdPASS) {
      continue;
    }

    if (command.target == DeviceTarget::StatusLed) {
      setStatusLed(app, command.enabled);
    } else {
      setRgbBeacon(app, command.enabled);
    }

    updateDeviceSnapshot(app, command.target, command.enabled);
    Serial.printf(
      "[Device] %s -> %s\r\n",
      command.target == DeviceTarget::StatusLed ? "Status LED" : "RGB Beacon",
      command.enabled ? "ON" : "OFF"
    );
  }
}

void cloudTask(void *pvParameters) {
  auto *app = static_cast<AppContext *>(pvParameters);
  uint32_t lastPublishMs = 0;
  uint32_t lastStationAttemptMs = 0;
  uint32_t publishCount = 0;
  bool attributesSent = false;
  bool missingCredentialsLogged = false;
  constexpr TickType_t serviceInterval = pdMS_TO_TICKS(200);

  for (;;) {
    const bool staConfigured = hasStationCredentials();
    const bool staConnected = ensureStationConnected(lastStationAttemptMs);
    bool coreIotConnected = false;

    if (!staConfigured) {
      if (!missingCredentialsLogged) {
        Serial.println("[CoreIOT] Waiting for STA WiFi credentials in include/cloud_credentials.h");
        missingCredentialsLogged = true;
      }
    } else if (staConnected) {
      missingCredentialsLogged = false;
      coreIotConnected = ensureCoreIotConnected(app, attributesSent);

      if (coreIotConnected) {
        if (!attributesSent) {
          publishCoreIotAttributes(app);
          attributesSent = true;
        }

        app->coreIotClient->loop();

        const uint32_t now = millis();
        if (now - lastPublishMs >= cloud_credentials::kTelemetryIntervalMs) {
          const DashboardSnapshot snapshot = copySnapshot(app);
          publishCoreIotTelemetry(app, snapshot);
          lastPublishMs = now;
          publishCount++;

          if (snapshot.hasReading) {
            Serial.printf(
              "[CoreIOT] Published #%lu T=%.1f C, H=%.1f %%\r\n",
              static_cast<unsigned long>(publishCount),
              snapshot.latestReading.temperatureC,
              snapshot.latestReading.humidityPercent
            );
          }
        }
      }
    }

    updateCloudSnapshot(app, {
      staConfigured,
      staConnected,
      coreIotConnected,
      publishCount,
    });

    vTaskDelay(serviceInterval);
  }
}

void webServerTask(void *pvParameters) {
  auto *app = static_cast<AppContext *>(pvParameters);
  constexpr TickType_t serviceInterval = pdMS_TO_TICKS(20);

  for (;;) {
    if (app->server != nullptr) {
      app->server->handleClient();
    }
    vTaskDelay(serviceInterval);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  auto *app = new AppContext{};
  app->wire = &Wire;
  app->lcd = nullptr;
  app->server = nullptr;
  app->rgbPixel = nullptr;
  app->cloudWifiClient = nullptr;
  app->cloudMqttClient = nullptr;
  app->coreIotClient = nullptr;
  app->sdaPin = GPIO_NUM_11;
  app->sclPin = GPIO_NUM_12;
  app->statusLedPin = LED_BUILTIN;
  app->rgbLedPin = RGB_LED;

  initializeSnapshot(app);

  if (!initializeChannels(app)) {
    Serial.println("[RTOS] Failed to create semaphore, mutex, or queue");
    while (true) {
      delay(1000);
    }
  }

  app->wire->begin(app->sdaPin, app->sclPin);
  app->sensor.begin();

  app->snapshot.lcdAvailable = initializeLcd(app);
  initializeRgbBeacon(app);
  initializeCoreIot(app);
  updateTinyMlSnapshot(app, evaluateTinyMlOnDevice());

  WiFi.mode(hasStationCredentials() ? WIFI_MODE_APSTA : WIFI_MODE_AP);
  WiFi.setSleep(false);
  startAccessPoint(app);
  showAccessPointInfo(app);
  delay(1400);

  configureWebServer(app);

  auto *normalTask = new DisplayTaskContext{app, &app->normalChannel};
  auto *warningTask = new DisplayTaskContext{app, &app->warningChannel};
  auto *criticalTask = new DisplayTaskContext{app, &app->criticalChannel};

  xTaskCreate(sensorTask, "SensorTask", 4096, app, 2, nullptr);
  xTaskCreate(inferenceTask, "TinyMLTask", 4096, app, 2, nullptr);
  xTaskCreate(displayTask, "DisplayNormal", 4096, normalTask, 1, nullptr);
  xTaskCreate(displayTask, "DisplayWarning", 4096, warningTask, 1, nullptr);
  xTaskCreate(displayTask, "DisplayCritical", 4096, criticalTask, 1, nullptr);
  xTaskCreate(deviceTask, "DeviceTask", 4096, app, 2, nullptr);
  xTaskCreate(cloudTask, "CloudTask", 8192, app, 1, nullptr);
  xTaskCreate(webServerTask, "WebServerTask", 8192, app, 1, nullptr);

  Serial.println("[System] Task 6 CoreIOT dashboard is running");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
