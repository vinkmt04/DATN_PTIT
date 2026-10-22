# HỆ THỐNG IOT NHÀ THÔNG MINH DỰA TRÊN MẠNG THREAD MESH, TÍCH HỢP BỘ ĐIỀU KHIỂN TRUNG TÂM BẰNG GIỌNG NÓI ỨNG DỤNG AI PHÂN TÁN (EDGE - CLOUD)

## Đặc tả tính năng sản phẩm
> **Đồ án tốt nghiệp** — Nhóm 2 sinh viên.
> **Thời gian thực hiện dự kiến:** 2 tháng.

---

## PHẦN 1: TỔNG QUAN SẢN PHẨM

Một hệ thống vạn vật kết nối (IoT) khép kín bao gồm:
- **01 Thiết bị trung tâm (Voice Gateway):** Tích hợp màn hình cảm ứng, Micro/Loa xử lý giọng nói. Đóng vai trò là Bộ định tuyến biên (OpenThread Border Router) điều phối toàn mạng lưới.
- **Nhiều Node thiết bị không dây (End-Devices):** Các module công tắc, đèn, cảm biến môi trường giao tiếp nội bộ qua chuẩn mạng Thread Mesh (IPv6).
- **Trí tuệ nhân tạo (AI) 2 lớp - "Local-First, Cloud-Fallback":** Mọi câu lệnh đều ưu tiên phân tích và xử lý tức thời tại biên (Edge AI). Chỉ khi câu lệnh mang ngữ nghĩa phức tạp mới được đẩy lên Đám mây (Cloud LLM) để suy luận. Giúp hệ thống phản hồi siêu tốc và hoạt động ổn định kể cả khi mất Internet.
- **Giám sát & Quản lý:** Đồng bộ trạng thái toàn cục theo thời gian thực qua giao thức MQTT trên nền tảng ThingsBoard.

---

## PHẦN 2: CHI TIẾT TÍNH NĂNG THIẾT BỊ TRUNG TÂM (VOICE GATEWAY)

### 2.1. Phần cứng
- **ESP32-S3 DevKit (8MB PSRAM) — Chip Chủ (Host):** Đảm nhiệm các tác vụ nặng: Thu/phát Audio I2S, chạy mô hình Edge AI, vẽ giao diện đồ họa, điều phối OpenThread Border Router (OTBR) và kết nối Wi-Fi/MQTT.
- **ESP32-C6 (hoặc H2) DevKit — Radio Co-Processor (RCP):** Cắm vào ESP32-S3 qua giao tiếp UART/SPI. Chuyên trách xử lý tín hiệu sóng vô tuyến 802.15.4 cho mạng Thread.
- **Màn hình TFT cảm ứng:** Hiển thị trực quan trạng thái các phòng, thông số môi trường, đồng hồ và cung cấp giao diện chạm để điều khiển trực tiếp.
- **Micro & Loa:** Micro I2S (INMP441) đa hướng + Amply I2S (MAX98357A) ra Loa.
- *(Lưu ý kiến trúc: Để tối đa hóa tài nguyên cho mô hình AI và OTBR, tính năng cập nhật từ xa OTA trên thiết bị trung tâm đã được loại bỏ để dành trọn bộ nhớ Flash).*

### 2.2. Luồng xử lý giọng nói "Local-First, Cloud Fallback"
**Nguyên tắc cốt lõi:** Tối ưu độ trễ và sự phụ thuộc vào mạng Internet.

1. **Wake-word (Luôn thức):** Chạy cục bộ trên ESP32-S3 (Huấn luyện qua Edge Impulse).
2. **Multi-Keyword Spotting (Lệnh cơ bản):** Khi nghe lệnh (VD: *"Bật đèn", "Tắt quạt"*), hệ thống đối chiếu với mô hình học máy cục bộ. Nếu độ tin cậy cao -> **Thực thi ngay lập tức**, không cần Internet.
3. **Cloud LLM (Lệnh phức tạp/Trò chuyện):** Nếu âm thanh không khớp mẫu cục bộ, Gateway gửi luồng Audio lên Cloud qua WebSocket (Sử dụng **Gemini Live API**). LLM tự bóc tách ngữ nghĩa:
   - Nếu là lệnh tự do (VD: *"Phòng nóng quá, bật giúp cái quạt"*): LLM trả về mã lệnh JSON để Gateway thực thi.
   - Nếu là câu hỏi thông tin: LLM trả về text, Gateway dùng TTS đọc ra Loa.
4. **Graceful Degradation (Kịch bản rớt mạng):** Nếu mất Internet, hệ thống báo lỗi với lệnh phức tạp, nhưng các lệnh cơ bản cục bộ vẫn hoạt động bình thường.

### 2.3. Command Dispatcher & Giao diện
- **Dispatcher:** Mọi lệnh (từ AI, Màn hình, hay Cloud) đều được tập hợp, tra bảng địa chỉ IPv6 và đóng gói thành bản tin CoAP gửi xuống Thread Mesh.
- **Màn hình cảm ứng:** Hỗ trợ điều khiển thủ công, cảnh báo rớt mạng Node, hiển thị trạng thái động.

---

## PHẦN 3: TÍNH NĂNG CÁC THIẾT BỊ CHẤP HÀNH (THREAD NODES)

| Chức năng Node | Chip đề xuất | Vai trò Thread | Mô tả kỹ thuật |
|---|---|---|---|
| **Công tắc thông minh** | ESP32-C6 / H2 | FTD (Router) | Điều khiển Relay, đo dòng điện tiêu thụ (ACS712/INA219) |
| **Đèn Dimming** | ESP32-C6 / H2 | FTD (Router) | Băm xung PWM điều chỉnh cường độ sáng |
| **Cảm biến môi trường**| ESP32-C6 / H2 | MTD (Sleepy Node) | Chạy pin. Đo nhiệt/ẩm (DHT22) + Ánh sáng (BH1750). Cập nhật định kỳ rồi ngủ sâu |

*Các thiết bị FTD (Full Thread Device) cắm điện lưới sẽ tự động trở thành các Router, giúp mở rộng độ phủ sóng mạng Mesh nội bộ và tự động định tuyến lại (self-healing) khi có một Node bị hỏng.*

---

## PHẦN 4: BẢN ĐỒ CÔNG NGHỆ TRÍ TUỆ NHÂN TẠO

| Thành phần AI | Nền tảng huấn luyện / Dịch vụ | Môi trường thực thi | Ghi chú |
|---|---|---|---|
| **Từ khóa đánh thức (Wake-word)** | Tự thu thập & Train qua **Edge Impulse** | Chạy Offline trên ESP32-S3 | Đòi hỏi phản hồi < 0.5s |
| **Nhận diện lệnh (Keyword-Spotting)** | Tự thu thập & Train qua **Edge Impulse** | Chạy Offline trên ESP32-S3 | Lệnh cơ bản (Bật/tắt) |
| **Phân tích ngữ nghĩa tự nhiên (NLU)** | Sử dụng API dịch vụ Cloud | **Gemini Live API** (LLM) | Dùng WebSocket streaming |

---

## PHẦN 5: KẾ HOẠCH THỰC HIỆN & CHỈ SỐ ĐÁNH GIÁ (2 THÁNG)

**Lộ trình triển khai (8 Tuần) - *Đã cập nhật tiến độ thực tế*:**
- **Tuần 1 - 2 (Core & Networking) [Đã hoàn thành]:** Xây dựng bộ khung Gateway ESP32-S3. Thiết lập mạng Thread Mesh sử dụng Host (ESP32-C6/S3) kết hợp RCP (ESP32-H2). Kết nối và kiểm thử thành công các Node Thread (Relay, Dimmer) qua giao thức CoAP/UDP với khả năng tự phục hồi từ NVS.
- **Tuần 3 - 4 (Edge AI & Wake-word) [Đã hoàn thành]:** Thu âm dataset đa dạng (6 người), huấn luyện và tối ưu hóa mô hình Wake-word `hey_cherry` trên Edge Impulse đạt độ chính xác 96.3%[cite: 11] (tích hợp Machine Learning inference chạy liên tục 7ms[cite: 10] trên ESP32-S3). Xây dựng thành công Máy trạng thái 4 pha và tích hợp luồng pre-warm WebSocket Gemini Live API.
- **Tuần 5 (Command Engine & Cloud AI):** Huấn luyện mô hình nhận diện lệnh cục bộ (Command Spotting - Tầng 2) cho 6 câu lệnh điều khiển thiết bị cốt lõi. Hoàn thiện tích hợp WebSockets kết nối Gemini Live API cho các truy vấn linh hoạt (Cloud Fallback).
- **Tuần 6 (UI & Peripheral Expansion):** Thiết kế giao diện LVGL cho màn hình TFT (ST7789V). Mở rộng thêm các Node cảm biến môi trường (nhiệt độ, độ ẩm) hoạt động ở chế độ Sleepy End Device.
- **Tuần 7 (Cloud IoT & Full Integration):** Chuyển đổi bộ thu phát RCP sang trực tiếp Gateway ESP32-S3 qua UART. Tích hợp giao thức MQTT và thiết kế Dashboard giám sát/điều khiển trên ThingsBoard.
- **Tuần 8 (Testing & Report):** Chạy thực nghiệm toàn hệ thống (Edge - Cloud - Thread Mesh), đo đạc các chỉ số KPI thực tế và hoàn thiện báo cáo Đồ án tốt nghiệp.

**Các chỉ số thực nghiệm cần đo đạc (KPIs):**
1. **Local-hit Rate:** Tỷ lệ % các câu lệnh cơ bản (`bật/tắt đèn, quạt, công tắc`) được xử lý thành công hoàn toàn tại biên (Edge AI) mà không cần gọi Cloud.
2. **Độ trễ toàn trình (Latency):** So sánh thời gian từ lúc người dùng kết thúc câu lệnh đến lúc thiết bị phản hồi thực tế giữa xử lý Offline (Edge AI + CoAP) và Online (Gemini Live API).
3. **Độ tin cậy của mạng Thread:** Thời gian mạng tự phục hồi (Self-healing) và định tuyến lại đường đi khi ngắt nguồn một Node Router trung gian trong mạng nhiều chặng (Multi-hop).

---

## PHẦN 6: PHÂN CÔNG NHÓM 2 NGƯỜI

- **Trần Văn Phúc — Mesh & Firmware Node thiết bị:** Quản lý mạng Thread, thiết lập Commissioning, code Firmware cho 3 loại Node, tối ưu năng lượng Node pin (MTD), và đo thực nghiệm mạng Mesh.
- **Nguyễn Đức Quang Vinh — Thiết bị trung tâm & AI:** Lập trình phần mềm ESP32-S3 (Audio pipeline Local-first/Cloud-fallback, Giao diện cảm ứng TFT, OTBR, Dispatcher). Phụ trách thu âm và huấn luyện Wake-word + Keyword-spotting, cấu hình API Gemini và MQTT/ThingsBoard.

---

## PHẦN 7: DANH SÁCH LINH KIỆN (BOM THAM KHẢO)

| Thành phần | Gợi ý linh kiện | Số lượng |
|---|---|---|
| Host trung tâm | ESP32-S3 DevKit (8MB PSRAM) | 1 |
| RCP Module | ESP32-C6 / H2 DevKit | 1 |
| Màn hình | TFT cảm ứng SPI (ILI9341/ST7789) | 1 |
| Module Audio | Mic I2S INMP441, Loa + Amp I2S MAX98357A | 1 bộ |
| RTC | DS3231 (Đồng bộ thời gian chuẩn) | 1 |
| Node thiết bị (Phát triển) | ESP32-C6 / H2 DevKit | 4-6 |
| Relay / Đo dòng | Relay 5V hoặc SSR + ACS712 / INA219 | Theo số Node ổ cắm |
| Cảm biến môi trường | DHT22 (Nhiệt/ẩm) + BH1750 (Ánh sáng) | Theo số Node cảm biến |
| Nguồn cấp Node MTD | Pin Li-Po + Mạch sạc xả | Tùy chọn |
