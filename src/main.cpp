#define LED_PIN 48
#define SDA_PIN GPIO_NUM_11
#define SCL_PIN GPIO_NUM_12
#define NUM_PIXELS 4
#define MY_LED 4

#include <WiFi.h>
#include <Arduino_MQTT_Client.h>
#include <ThingsBoard.h>
#include "DHT20.h"
#include "Wire.h"
#include <ArduinoOTA.h>

// ================== CẤU HÌNH PHẦN CỨNG ==================

// LED đơn cho Task 1 – mình dùng chân R của module RGB

// DHT22 (cảm biến T/H)


// NeoPixel (WS2812) cho Task 2
// Chân S (DIN) của dải led nối vào NEO_PIN

// ================== RTOS HANDLE & BIẾN DÙNG CHUNG ==================

TaskHandle_t      taskHandleSensor   = NULL;
TaskHandle_t      taskHandleLED      = NULL;
TaskHandle_t      taskHandleNeoPixel = NULL;

SemaphoreHandle_t xTempSemaphore     = NULL;   // cho Task 1 (LED theo T)
SemaphoreHandle_t xHumSemaphore      = NULL;   // cho Task 2 (Neo theo H)

volatile float g_temperature = 0.0f;
volatile float g_humidity    = 0.0f;

// ================== HÀM TIỆN ÍCH CHO LED ĐƠN (TASK 1) ==================

// Nếu LED là common cathode (chân chung GND) thì ON = HIGH.
// Nếu common anode (chân chung 3V3) thì đổi LED_ACTIVE_HIGH = false.
const bool LED_ACTIVE_HIGH = true;

void ledOn()
{
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? HIGH : LOW);
}

void ledOff()
{
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? LOW : HIGH);
}

// ---- 3 PATTERN NHẤP NHÁY TƯƠNG ỨNG 3 VÙNG NHIỆT ĐỘ ----

// COOL: T < 25°C  → nháy chậm, 2 lần
void patternCool()
{
  Serial.println("[LED] Mode: COOL (slow blink, 2 times)");
  for (int i = 0; i < 2; ++i)
  {
    ledOn();
    vTaskDelay(pdMS_TO_TICKS(800));
    ledOff();
    vTaskDelay(pdMS_TO_TICKS(800));
  }
}

// NORMAL: 25°C ≤ T < 30°C → nháy trung bình, 4 lần
void patternNormal()
{
  Serial.println("[LED] Mode: NORMAL (medium blink, 4 times)");
  for (int i = 0; i < 4; ++i)
  {
    ledOn();
    vTaskDelay(pdMS_TO_TICKS(300));
    ledOff();
    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

// HOT: T ≥ 30°C → nháy nhanh, 6 lần
void patternHot()
{
  Serial.println("[LED] Mode: HOT (fast blink, 6 times)");
  for (int i = 0; i < 6; ++i)
  {
    ledOn();
    vTaskDelay(pdMS_TO_TICKS(120));
    ledOff();
    vTaskDelay(pdMS_TO_TICKS(120));
  }
}

// ================== HÀM VẼ THANH ĐỘ ẨM 10 MỨC (TASK 2) ==================
//
// Mapping:
//   - LED 0..2  (mức 1–3):  xanh lá  – độ ẩm thấp (ok)
//   - LED 3..6  (mức 4–7):  vàng     – trung bình
//   - LED 7..9  (mức 8–10): đỏ       – ẩm cao / nguy hiểm
//
void showHumidityBar(float hum)
{
  // Quy đổi 0–100% -> 0..NEO_COUNT (0..10)
  int level = (int)round((hum / 100.0f) * NEO_COUNT);
  level = constrain(level, 0, NEO_COUNT);

  strip.clear();

  for (int i = 0; i < level; ++i)
  {
    uint8_t r, g, b;

    if (i < 3) {
      // mức 1-3 → xanh lá
      r = 0;   g = 255; b = 0;
    } else if (i < 7) {
      // mức 4-7 → vàng
      r = 255; g = 255; b = 0;
    } else {
      // mức 8-10 → đỏ
      r = 255; g = 0;   b = 0;
    }

    strip.setPixelColor(i, strip.Color(r, g, b));
  }

  // các led từ "level" tới 9 mặc định là off
  strip.show();

  Serial.print("[NeoPixel] hum = ");
  Serial.print(hum);
  Serial.print(" %, level = ");
  Serial.println(level);
}

// ================== TASK ĐỌC DHT22 (SENSOR TASK) ==================

void vTaskSensor(void *pvParameters)
{
  (void)pvParameters;

  for (;;)
  {
    float t = dht.readTemperature();  // °C
    float h = dht.readHumidity();     // %

    if (isnan(t) || isnan(h))
    {
      Serial.println("[Sensor] Failed to read from DHT22!");
      // không give semaphore nếu đọc lỗi
    }
    else
    {
      g_temperature = t;
      g_humidity    = h;

      Serial.print("[Sensor] Temperature = ");
      Serial.print(g_temperature);
      Serial.print(" *C, Humidity = ");
      Serial.print(g_humidity);
      Serial.println(" %");

      // Thông báo cho Task 1 và Task 2
      xSemaphoreGive(xTempSemaphore);
      xSemaphoreGive(xHumSemaphore);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));   // chu kỳ đọc ~2s
  }
}

// ================== TASK LED ĐƠN (TASK 1) ==================

void vTaskLED(void *pvParameters)
{
  (void)pvParameters;

  pinMode(LED_PIN, OUTPUT);
  ledOff();

  for (;;)
  {
    if (xSemaphoreTake(xTempSemaphore, portMAX_DELAY) == pdTRUE)
    {
      float temp = g_temperature;

      Serial.print("[LED] Received new temperature = ");
      Serial.print(temp);
      Serial.println(" *C");

      if (temp < 25.0f)
      {
        patternCool();
      }
      else if (temp < 30.0f)
      {
        patternNormal();
      }
      else
      {
        patternHot();
      }

      ledOff();
    }
  }
}

// ================== TASK NEOPIXEL (TASK 2) ==================

void vTaskNeoPixel(void *pvParameters)
{
  (void)pvParameters;

  strip.begin();
  strip.setBrightness(60);    // giảm chói, có thể chỉnh 0–255
  strip.clear();
  strip.show();

  for (;;)
  {
    if (xSemaphoreTake(xHumSemaphore, portMAX_DELAY) == pdTRUE)
    {
      float hum = g_humidity;
      Serial.print("[NeoPixel] Received new humidity = ");
      Serial.print(hum);
      Serial.println(" %");

      // vẽ thanh mức độ ẩm
      showHumidityBar(hum);
    }
  }
}

// ================== SETUP & LOOP ==================

void setup()
{
  Serial.begin(115200);
  delay(2000);

  Serial.println("=== Task 1 & Task 2: DHT22 + Single LED (Temp) + NeoPixel 10-level Humidity Bar ===");

  dht.begin();

  // Tạo binary semaphore
  xTempSemaphore = xSemaphoreCreateBinary();
  xHumSemaphore  = xSemaphoreCreateBinary();

  if (xTempSemaphore == NULL || xHumSemaphore == NULL)
  {
    Serial.println("Failed to create semaphores!");
    while (1) { delay(1000); }
  }

  BaseType_t result;

  // Task đọc sensor
  result = xTaskCreate(
    vTaskSensor,
    "SensorTask",
    4096,
    NULL,
    3,                    // ưu tiên cao nhất
    &taskHandleSensor
  );
  if (result != pdPASS) Serial.println("Failed to create SensorTask");

  // Task LED theo nhiệt độ (Task 1)
  result = xTaskCreate(
    vTaskLED,
    "LEDTask",
    4096,
    NULL,
    2,
    &taskHandleLED
  );
  if (result != pdPASS) Serial.println("Failed to create LEDTask");

  // Task NeoPixel theo độ ẩm (Task 2)
  result = xTaskCreate(
    vTaskNeoPixel,
    "NeoTask",
    4096,
    NULL,
    1,
    &taskHandleNeoPixel
  );
  if (result != pdPASS) Serial.println("Failed to create NeoTask");
}

void loop()
{
  // Không dùng loop; mọi thứ chạy trong FreeRTOS task
  vTaskDelay(pdMS_TO_TICKS(1000));
}
