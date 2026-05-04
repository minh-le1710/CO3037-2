#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>
#include "soc/soc_caps.h"

#define USB_VID 0x2341
#define USB_PID 0x0070

#ifndef __cplusplus
#define constexpr const
#endif

static constexpr uint8_t D0 = 44;
static constexpr uint8_t D1 = 43;
static constexpr uint8_t D2 = 5;
static constexpr uint8_t D3 = 6;
static constexpr uint8_t D4 = 7;
static constexpr uint8_t D5 = 8;
static constexpr uint8_t D6 = 9;
static constexpr uint8_t D7 = 10;
static constexpr uint8_t D8 = 17;
static constexpr uint8_t D9 = 18;
static constexpr uint8_t D10 = 21;
static constexpr uint8_t D11 = 38;
static constexpr uint8_t D12 = 47;
static constexpr uint8_t D13 = 48;

static constexpr uint8_t RGB_LED = 45;

static constexpr uint8_t A0 = 1;
static constexpr uint8_t A1 = 2;
static constexpr uint8_t A2 = 3;
static constexpr uint8_t A3 = 4;
static constexpr uint8_t A4 = 11;
static constexpr uint8_t A5 = 12;
static constexpr uint8_t A6 = 13;
static constexpr uint8_t A7 = 14;

static constexpr uint8_t LED_BUILTIN = D13;

static constexpr uint8_t TX = D1;
static constexpr uint8_t RX = D0;
static constexpr uint8_t RTS = RGB_LED;
static constexpr uint8_t CTS = D3;
static constexpr uint8_t DTR = A0;
static constexpr uint8_t DSR = D4;

static constexpr uint8_t SS = D10;
static constexpr uint8_t MOSI = D11;
static constexpr uint8_t MISO = D12;
static constexpr uint8_t SCK = D13;

static constexpr uint8_t SDA = A4;
static constexpr uint8_t SCL = A5;

#define PIN_I2S_SCK     D7
#define PIN_I2S_FS      D8
#define PIN_I2S_SD      D9
#define PIN_I2S_SD_OUT  D9
#define PIN_I2S_SD_IN   D10

#ifndef __cplusplus
#undef constexpr
#endif

#endif
