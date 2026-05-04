# Task 6 - Data Publishing to CoreIOT Cloud Server

## Mục tiêu

- Publish dữ liệu cảm biến từ Yolo UNO / ESP32-S3 lên CoreIOT server `app.coreiot.io`
- Thiết bị phải có chế độ Station để đi Internet
- Dùng đúng access token của device đã đăng ký trên CoreIOT

## Cách triển khai trong firmware

- Giữ lại các task cũ của Task 3, 4, 5:
  - đọc DHT20
  - suy luận TinyML
  - hiển thị LCD
  - web dashboard
  - điều khiển 2 thiết bị
- Thêm `cloudTask` để chạy riêng phần mạng cloud:
  1. kiểm tra cấu hình STA
  2. kết nối WiFi ở chế độ Station
  3. kết nối MQTT tới CoreIOT
  4. gửi telemetry định kỳ mỗi 5 giây

## File cấu hình

- WiFi STA và token được đặt trong `include/cloud_credentials.h`
- CoreIOT access token đã cấu hình:
  - device token: đã gắn theo token bạn cung cấp
- Hai trường còn phải điền để publish thật:
  - `kStaSsid`
  - `kStaPassword`

## Telemetry được gửi

- `temperature`
- `humidity`
- `tinyml_confidence`
- `tinyml_state`
- `tinyml_accuracy`
- `rssi`

## Trạng thái hiện tại

- Build thành công với `pio run`
- Upload thành công lên `COM29`
- Log boot xác nhận `cloudTask` đã chạy
- Do chưa có SSID/password STA nên board đang chờ cấu hình WiFi:
  - xem `docs/task6_serial_log.txt`

## Kết luận

Task 6 đã được tích hợp xong về mặt code, token CoreIOT đã được cấu hình, và firmware đã sẵn sàng publish dữ liệu ngay khi có thông tin WiFi STA hợp lệ để kết nối Internet.
