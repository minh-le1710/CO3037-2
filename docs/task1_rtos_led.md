# Task 1 - Single LED Blink with Temperature Conditions

## Mục tiêu

- Dùng 1 LED ở chân `GPIO 48`
- Đọc nhiệt độ từ cảm biến `DHT20`
- Có ít nhất 3 kiểu nháy đèn khác nhau theo điều kiện nhiệt độ
- Dùng semaphore để đồng bộ giữa các task RTOS

## Phần cứng sử dụng

- Board: `Yolo UNO (ESP32-S3)`
- LED điều khiển: `GPIO 48`
- DHT20 qua I2C:
  - `SDA = GPIO 11`
  - `SCL = GPIO 12`

## Điều kiện nhiệt độ và hành vi LED

1. `COOL` khi `T < 24 C`
   - LED nháy chậm: `1 giây bật`, `1 giây tắt`
2. `NORMAL` khi `24 C <= T < 29 C`
   - LED nháy đôi: `180 ms bật`, `180 ms tắt`, `180 ms bật`, `900 ms tắt`
3. `HOT` khi `T >= 29 C`
   - LED nháy nhanh: `120 ms bật`, `120 ms tắt`

## Thiết kế RTOS

### 1. SensorTask

- Đọc DHT20 mỗi 2 giây
- Phân loại nhiệt độ thành `COOL / NORMAL / HOT`
- Ghi trạng thái mới vào vùng dữ liệu dùng chung
- Nếu vùng nhiệt độ thay đổi, task này sẽ `give` binary semaphore `behaviorChanged`

### 2. LedTask

- Ban đầu chờ semaphore để biết đã có dữ liệu cảm biến đầu tiên
- Sau đó chạy pattern nháy tương ứng với vùng nhiệt độ hiện tại
- Trong từng khoảng delay nháy đèn, task không `delay` mù mà dùng `xSemaphoreTake(..., timeout)`
- Nếu `SensorTask` báo có vùng nhiệt độ mới, `LedTask` bị đánh thức ngay để đổi pattern

## Logic semaphore

- `stateMutex`:
  - bảo vệ dữ liệu dùng chung gồm nhiệt độ, độ ẩm, trạng thái nhiệt độ
  - tránh race condition giữa `SensorTask` và `LedTask`
- `behaviorChanged`:
  - là binary semaphore dùng để báo sự kiện “nhiệt độ đã đổi vùng”
  - giúp `LedTask` phản ứng ngay khi điều kiện nhiệt độ đổi, không cần polling liên tục

## Kết luận

Chương trình đáp ứng đúng yêu cầu Task 1:

- có 3 hành vi LED khác nhau theo nhiệt độ
- dùng RTOS
- dùng semaphore rõ ràng để đồng bộ task đọc cảm biến và task nháy LED
- code đã được chú thích trực tiếp để giải thích điều kiện và cơ chế semaphore
