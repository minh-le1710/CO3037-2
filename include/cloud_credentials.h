#pragma once

#include <stdint.h>

namespace cloud_credentials {

constexpr char kStaSsid[] = "iPhone 16 Plus";
constexpr char kStaPassword[] = "minh1710";

constexpr char kCoreIotServer[] = "app.coreiot.io";
constexpr uint16_t kCoreIotPort = 1883U;
constexpr char kCoreIotToken[] = "hm0jfn0xoi43gsz7c1ed";

constexpr uint32_t kTelemetryIntervalMs = 5000U;
constexpr uint32_t kReconnectDelayMs = 8000U;

}  // namespace cloud_credentials
