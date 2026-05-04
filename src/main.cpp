#include <Arduino.h>
#include <Wire.h>

#include "DHT20.h"

enum class TemperatureBehavior : uint8_t {
  Cool,
  Normal,
  Hot,
};

struct SharedState {
  float latestTemperatureC;
  float latestHumidityPercent;
  TemperatureBehavior behavior;
  bool hasReading;
};

struct AppContext {
  TwoWire *wire;
  DHT20 sensor;
  SemaphoreHandle_t stateMutex;
  SemaphoreHandle_t behaviorChanged;
  SharedState state;
  int sdaPin;
  int sclPin;
  uint8_t ledPin;
};

TemperatureBehavior classifyTemperature(float temperatureC) {
  if (temperatureC < 24.0f) {
    return TemperatureBehavior::Cool;
  }

  if (temperatureC < 29.0f) {
    return TemperatureBehavior::Normal;
  }

  return TemperatureBehavior::Hot;
}

const char *behaviorName(TemperatureBehavior behavior) {
  switch (behavior) {
    case TemperatureBehavior::Cool:
      return "COOL";
    case TemperatureBehavior::Normal:
      return "NORMAL";
    case TemperatureBehavior::Hot:
      return "HOT";
  }

  return "UNKNOWN";
}

void setLed(uint8_t ledPin, bool enabled) {
  digitalWrite(ledPin, enabled ? HIGH : LOW);
}

SharedState copySharedState(AppContext *app) {
  SharedState state = {};

  if (xSemaphoreTake(app->stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    state = app->state;
    xSemaphoreGive(app->stateMutex);
  }

  return state;
}

bool updateSharedState(
  AppContext *app,
  float temperatureC,
  float humidityPercent,
  TemperatureBehavior behavior
) {
  bool behaviorChanged = false;

  if (xSemaphoreTake(app->stateMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false;
  }

  behaviorChanged = !app->state.hasReading || app->state.behavior != behavior;
  app->state.latestTemperatureC = temperatureC;
  app->state.latestHumidityPercent = humidityPercent;
  app->state.behavior = behavior;
  app->state.hasReading = true;

  xSemaphoreGive(app->stateMutex);
  return behaviorChanged;
}

bool waitForBehaviorChange(
  AppContext *app,
  TickType_t waitTicks,
  TemperatureBehavior &activeBehavior
) {
  if (xSemaphoreTake(app->behaviorChanged, waitTicks) != pdTRUE) {
    return false;
  }

  const SharedState state = copySharedState(app);
  if (state.hasReading) {
    activeBehavior = state.behavior;
  }

  return true;
}

// SensorTask reads DHT20 periodically, classifies the current temperature band,
// then "gives" the binary semaphore whenever the behavior changes.
// The semaphore is the synchronization point that wakes the LED task immediately.
void sensorTask(void *pvParameters) {
  auto *app = static_cast<AppContext *>(pvParameters);
  constexpr TickType_t samplePeriod = pdMS_TO_TICKS(2000);

  app->wire->begin(app->sdaPin, app->sclPin);
  app->sensor.begin();
  vTaskDelay(pdMS_TO_TICKS(600));

  for (;;) {
    app->sensor.read();

    const float temperatureC = app->sensor.getTemperature();
    const float humidityPercent = app->sensor.getHumidity();

    if (isnan(temperatureC) || isnan(humidityPercent)) {
      Serial.println("[Sensor] Failed to read DHT20");
      vTaskDelay(samplePeriod);
      continue;
    }

    const TemperatureBehavior behavior = classifyTemperature(temperatureC);
    const bool changed = updateSharedState(app, temperatureC, humidityPercent, behavior);

    Serial.printf(
      "[Sensor] T=%.1f C, H=%.1f %% -> %s\r\n",
      temperatureC,
      humidityPercent,
      behaviorName(behavior)
    );

    if (changed) {
      Serial.printf("[Sync] Behavior changed, release semaphore for %s\r\n", behaviorName(behavior));
      xSemaphoreGive(app->behaviorChanged);
    }

    vTaskDelay(samplePeriod);
  }
}

// LedTask waits on the semaphore until the first temperature band is available.
// After that it blinks using the pattern of the active band.
// During every ON/OFF delay it waits on the same semaphore with timeout, so a new
// temperature band can interrupt the current pattern without polling aggressively.
void ledTask(void *pvParameters) {
  auto *app = static_cast<AppContext *>(pvParameters);
  TemperatureBehavior activeBehavior = TemperatureBehavior::Cool;

  setLed(app->ledPin, false);

  for (;;) {
    const SharedState currentState = copySharedState(app);
    if (!currentState.hasReading) {
      xSemaphoreTake(app->behaviorChanged, portMAX_DELAY);
      continue;
    }

    activeBehavior = currentState.behavior;

    switch (activeBehavior) {
      case TemperatureBehavior::Cool:
        setLed(app->ledPin, true);
        if (waitForBehaviorChange(app, pdMS_TO_TICKS(1000), activeBehavior)) {
          setLed(app->ledPin, false);
          continue;
        }

        setLed(app->ledPin, false);
        waitForBehaviorChange(app, pdMS_TO_TICKS(1000), activeBehavior);
        break;

      case TemperatureBehavior::Normal:
        setLed(app->ledPin, true);
        if (waitForBehaviorChange(app, pdMS_TO_TICKS(180), activeBehavior)) {
          setLed(app->ledPin, false);
          continue;
        }

        setLed(app->ledPin, false);
        if (waitForBehaviorChange(app, pdMS_TO_TICKS(180), activeBehavior)) {
          continue;
        }

        setLed(app->ledPin, true);
        if (waitForBehaviorChange(app, pdMS_TO_TICKS(180), activeBehavior)) {
          setLed(app->ledPin, false);
          continue;
        }

        setLed(app->ledPin, false);
        waitForBehaviorChange(app, pdMS_TO_TICKS(900), activeBehavior);
        break;

      case TemperatureBehavior::Hot:
        setLed(app->ledPin, true);
        if (waitForBehaviorChange(app, pdMS_TO_TICKS(120), activeBehavior)) {
          setLed(app->ledPin, false);
          continue;
        }

        setLed(app->ledPin, false);
        waitForBehaviorChange(app, pdMS_TO_TICKS(120), activeBehavior);
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  auto *app = new AppContext{};
  app->wire = &Wire;
  app->stateMutex = xSemaphoreCreateMutex();
  app->behaviorChanged = xSemaphoreCreateBinary();
  app->sdaPin = 11;
  app->sclPin = 12;
  app->ledPin = 48;
  app->state = {0.0f, 0.0f, TemperatureBehavior::Cool, false};

  if (app->stateMutex == nullptr || app->behaviorChanged == nullptr) {
    Serial.println("[RTOS] Failed to create semaphore");
    while (true) {
      delay(1000);
    }
  }

  pinMode(app->ledPin, OUTPUT);
  setLed(app->ledPin, false);

  Serial.println("[System] Task 1 RTOS temperature blink is starting");
  Serial.println("[Rule] COOL   : T < 24 C -> slow blink");
  Serial.println("[Rule] NORMAL : 24 C <= T < 29 C -> double blink");
  Serial.println("[Rule] HOT    : T >= 29 C -> fast blink");

  xTaskCreate(sensorTask, "SensorTask", 4096, app, 2, nullptr);
  xTaskCreate(ledTask, "LedTask", 3072, app, 1, nullptr);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
