#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#include "DHT20.h"

enum class DisplayState : uint8_t {
  Normal,
  Warning,
  Critical,
};

struct SensorReading {
  float temperatureC;
  float humidityPercent;
  DisplayState state;
};

struct StateChannel {
  SemaphoreHandle_t semaphore;
  QueueHandle_t queue;
  const char *title;
  const char *note;
};

struct AppContext {
  TwoWire *wire;
  DHT20 sensor;
  LiquidCrystal_I2C *lcd;
  SemaphoreHandle_t lcdMutex;
  StateChannel normalChannel;
  StateChannel warningChannel;
  StateChannel criticalChannel;
  int sdaPin;
  int sclPin;
};

struct DisplayTaskContext {
  AppContext *app;
  StateChannel *channel;
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
  snprintf(line2, sizeof(line2), "H:%4.1f%% %-6s", reading.humidityPercent, channel.note);

  printPaddedLine(*app->lcd, 0, line1);
  printPaddedLine(*app->lcd, 1, line2);

  xSemaphoreGive(app->lcdMutex);
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

    SensorReading reading = {
      temperatureC,
      humidityPercent,
      classifyReading(temperatureC, humidityPercent),
    };

    StateChannel *targetChannel = selectChannel(app, reading.state);
    if (targetChannel != nullptr) {
      xQueueOverwrite(targetChannel->queue, &reading);
      xSemaphoreGive(targetChannel->semaphore);
    }

    Serial.printf("[Sensor] T=%.1f C, H=%.1f %%\r\n", temperatureC, humidityPercent);
    vTaskDelay(samplePeriod);
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

bool initializeLcd(AppContext *app) {
  const uint8_t lcdAddress = detectLcdAddress(*app->wire);
  if (lcdAddress == 0) {
    Serial.println("[LCD] No supported I2C LCD found");
    return false;
  }

  app->lcd = new LiquidCrystal_I2C(lcdAddress, 16, 2);
  app->lcd->init();
  app->lcd->backlight();

  printPaddedLine(*app->lcd, 0, "Task 3 Monitor");
  printPaddedLine(*app->lcd, 1, "Waiting sensor");

  Serial.printf("[LCD] Connected at 0x%02X\r\n", lcdAddress);
  return true;
}

bool initializeChannels(AppContext *app) {
  app->lcdMutex = xSemaphoreCreateMutex();
  app->normalChannel = {xSemaphoreCreateBinary(), xQueueCreate(1, sizeof(SensorReading)), "NORMAL", "OK"};
  app->warningChannel = {xSemaphoreCreateBinary(), xQueueCreate(1, sizeof(SensorReading)), "WARNING", "CHECK"};
  app->criticalChannel = {xSemaphoreCreateBinary(), xQueueCreate(1, sizeof(SensorReading)), "CRITICAL", "ALERT"};

  return app->lcdMutex != nullptr &&
         app->normalChannel.semaphore != nullptr &&
         app->warningChannel.semaphore != nullptr &&
         app->criticalChannel.semaphore != nullptr &&
         app->normalChannel.queue != nullptr &&
         app->warningChannel.queue != nullptr &&
         app->criticalChannel.queue != nullptr;
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  auto *app = new AppContext{};
  app->wire = &Wire;
  app->lcd = nullptr;
  app->sdaPin = GPIO_NUM_11;
  app->sclPin = GPIO_NUM_12;

  if (!initializeChannels(app)) {
    Serial.println("[RTOS] Failed to create semaphore or queue");
    while (true) {
      delay(1000);
    }
  }

  app->wire->begin(app->sdaPin, app->sclPin);
  app->sensor.begin();

  initializeLcd(app);

  auto *normalTask = new DisplayTaskContext{app, &app->normalChannel};
  auto *warningTask = new DisplayTaskContext{app, &app->warningChannel};
  auto *criticalTask = new DisplayTaskContext{app, &app->criticalChannel};

  xTaskCreate(sensorTask, "SensorTask", 4096, app, 2, nullptr);
  xTaskCreate(displayTask, "DisplayNormal", 4096, normalTask, 1, nullptr);
  xTaskCreate(displayTask, "DisplayWarning", 4096, warningTask, 1, nullptr);
  xTaskCreate(displayTask, "DisplayCritical", 4096, criticalTask, 1, nullptr);

  Serial.println("[System] Task 3 monitor is running");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
