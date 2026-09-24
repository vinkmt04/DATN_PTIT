# HỆ THỐNG IOT DỰA TRÊN MẠNG THREAD MESH, TÍCH HỢP BỘ ĐIỀU KHIỂN TRUNG TÂM BẰNG GIỌNG NÓI ỨNG DỤNG AI TẠI BIÊN VÀ ĐÁM MÂY
## Đặc tả tính năng sản phẩm

> Đồ án tốt nghiệp — nhóm 2 người, thời gian thực hiện 4-5 tháng.
> Định vị sản phẩm: một trợ lý giọng nói kiểu Google Home/Alexa, nhưng mạng thiết bị phía sau (điều khiển đèn, rèm, ổ cắm, cảm biến) do nhóm tự thiết kế từ giao thức tầng thấp (Thread Mesh, OpenThread), không dùng nền tảng Matter/Zigbee có sẵn — đây là điểm khác biệt kỹ thuật (USP) so với sản phẩm thương mại.

---

## PHẦN 1: TỔNG QUAN SẢN PHẨM

Một trợ lý giọng nói thông minh gồm:
- **1 Thiết bị trung tâm (hình thức giống loa thông minh)**: màn hình cảm ứng, mic/loa, xử lý giọng nói, đồng thời đóng vai trò Border Router cầu nối Thread Mesh ↔ WiFi/Cloud.
- **Nhiều node thiết bị không dây** (đèn, ổ cắm, rèm, cảm biến môi trường) giao tiếp qua Thread Mesh do nhóm tự xây dựng.
- **AI 2 lớp, xử lý theo nguyên tắc "cục bộ trước" (local-first)**: mọi lệnh đều thử xử lý tại chỗ trước bằng mô hình tự huấn luyện; chỉ đẩy lên Cloud LLM khi lệnh không khớp mẫu cục bộ — vừa nhanh, vừa tiết kiệm, vừa không phụ thuộc mạng cho việc thường ngày.
- **Giám sát/điều khiển từ xa** qua MQTT + ThingsBoard.

---

## PHẦN 2: TÍNH NĂNG THIẾT BỊ TRUNG TÂM

### 2.1. Phần cứng
- **ESP32-S3 DevKit** (8MB PSRAM) — chip chủ (Host): xử lý audio, AI, giao diện, Thread Border Router, MQTT client. Dùng bản DevKit (không phải SuperMini) để tận dụng 2 cổng USB-C (UART debug + USB-JTAG tích hợp), cần thiết cho việc debug pipeline audio+Thread+AI phức tạp.
- **ESP32-H2 DevKit** — RCP (Radio Co-Processor): xử lý tầng radio 802.15.4 cho Thread, nối UART với ESP32-S3.
- **Màn hình TFT cảm ứng**: trạng thái phòng/thiết bị, đồng hồ, thông tin môi trường, thao tác chạm điều khiển trực tiếp.
- **Mic I2S (INMP441) + loa/amp I2S (MAX98357A)**.
- **RTC (DS3231)**: đồng bộ giờ cho toàn hệ thống automation theo thời gian.

### 2.2. Điều khiển bằng giọng nói — pipeline xử lý "Local-First, Cloud Fallback"

**Nguyên tắc cốt lõi:** luôn thử xử lý cục bộ trước, bất kể có mạng hay không — chỉ gọi Cloud khi thật sự cần, để tối ưu độ trễ, chi phí, và giảm phụ thuộc mạng cho các lệnh thường ngày.

```
Wake-word kích hoạt (edge, luôn chạy, tự train qua Edge Impulse)
    → Thu audio câu lệnh
    → Chạy Multi-keyword-spotting cục bộ (tự train qua Edge Impulse, chạy trên ESP32-S3)
        ├── [Khớp, độ tin cậy cao] → Thực thi NGAY tại chỗ, KHÔNG gọi Cloud
        │     (độ trễ thấp, không phụ thuộc mạng, không tốn chi phí API)
        │
        └── [Không khớp / độ tin cậy thấp]
                ├── [Có mạng] → Gửi audio lên Cloud LLM API qua function calling
                │      LLM tự quyết định:
                │        - Khớp 1 hàm điều khiển đã định nghĩa → trả JSON, hệ thống thực thi
                │          (dieu_khien_thiet_bi, dat_hen_gio, tao_scene...)
                │        - Không khớp hàm nào → LLM tự trả lời tự do (hỏi đáp thông tin,
                │          trò chuyện) → đọc thẳng qua TTS
                │
                └── [Mất mạng] → Báo "không hiểu lệnh, vui lòng nói theo mẫu có sẵn"
                       (giới hạn Graceful Degradation, cần nêu rõ trong báo cáo)
```

**Tập lệnh xử lý cục bộ (multi-keyword-spotting):** "bật đèn", "tắt đèn", "mở rèm", "đóng rèm", "bật quạt", "tắt quạt"... — các lệnh đơn giản, tần suất dùng cao nhất, không cần điều kiện phức tạp.

**Lệnh cần Cloud LLM:** câu điều kiện phức tạp ("bật điều hòa nếu phòng lạnh"), hẹn giờ tùy biến, tạo/sửa scene bằng giọng nói, hỏi đáp thông tin chung.

**Giới hạn kỹ thuật cần nêu trong báo cáo:** keyword-spotting khớp khá "cứng" với đúng cụm từ đã huấn luyện và vài biến thể gần giống — nếu người dùng diễn đạt khác đi nhiều (ví dụ "làm ơn bật giúp tôi cái đèn"), nhiều khả năng không khớp và rơi xuống Cloud dù bản chất vẫn là lệnh đơn giản. Đây là giới hạn tự nhiên của keyword-spotting so với NLU đầy đủ, không phải lỗi thiết kế.

### 2.3. Command Dispatcher & phản hồi
- Với lệnh điều khiển thiết bị (dù xử lý cục bộ hay qua Cloud): tra bảng node theo phòng/loại thiết bị, đóng gói CoAP, gửi qua ESP32-H2 xuống Thread Mesh.
- Phản hồi: TTS qua loa + hiển thị trên màn hình.

### 2.4. Giao diện màn hình cảm ứng
- Trang tổng quan trạng thái node theo phòng.
- Điều khiển chạm trực tiếp (không cần giọng nói).
- Tạo/quản lý Scene và lịch hẹn giờ bằng tay.
- Cảnh báo node mất kết nối/pin yếu.

### 2.5. Scene tự động gợi ý bằng AI — tính năng MỞ RỘNG
> Làm sau khi hoàn thành phần lõi. Ghi log lịch sử lệnh, dùng association rule mining (Apriori/FP-Growth) phát hiện pattern lặp lại, đề xuất gộp scene.

### 2.6. Kết nối MQTT & ThingsBoard
- Publish trạng thái toàn bộ node lên ThingsBoard theo thời gian thực.
- Subscribe lệnh điều khiển từ xa, dùng chung luồng Dispatcher.
- Dashboard: biểu đồ môi trường, lịch sử thiết bị, cảnh báo, công suất tiêu thụ.

### 2.7. Phân tích môi trường & bản tin cá nhân hóa — tính năng phụ
- Phân tích chất lượng môi trường phòng (nhiệt/ẩm/CO2) theo thời gian.
- Đọc bản tin buổi sáng khi tới giờ hẹn (tận dụng `dat_hen_gio`, không cần logic riêng).

---

## PHẦN 3: TÍNH NĂNG NODE THIẾT BỊ (Thread Mesh, wireless, độc lập)

| Node | Chip đề xuất | Vai trò Thread | Chức năng |
|---|---|---|---|
| Ổ cắm/relay thông minh | ESP32-H2 (DevKit khi phát triển) | FTD (cắm điện) | Bật/tắt thiết bị cắm vào, đo dòng điện tiêu thụ (ACS712/INA219) |
| Đèn dimming | ESP32-H2 (DevKit khi phát triển) | FTD | Điều chỉnh độ sáng qua PWM |
| Cảm biến môi trường phòng | ESP32-H2 (DevKit khi phát triển) | **MTD** (sleepy end device, chạy pin) | Nhiệt/ẩm (DHT22/SHT31) + ánh sáng (BH1750), gửi định kỳ rồi ngủ |
| Rèm/mành tự động | ESP32-H2 (hoặc C6 nếu cần vai Router mạnh) | FTD | Motor kéo rèm, phản hồi vị trí qua limit switch/encoder |

**Ghi chú phần cứng:** giai đoạn phát triển/debug dùng bản DevKit cho mọi node (giảm rủi ro không chắc chắn khi còn làm quen với Thread, có jumper đo dòng chính xác cho thí nghiệm pin). Giai đoạn hoàn thiện sản phẩm cuối có thể cân nhắc chuyển sang SuperMini (từ nhà bán uy tín, đã sửa lỗi LDO/RGB) hoặc DevKitM-1 để gọn nhẹ hơn khi đóng hộp thật.

**Tự động hóa phân tán (không qua thiết bị trung tâm):** với cặp node phù hợp, thiết lập logic tự động cục bộ trực tiếp trong mesh — minh chứng khả năng hoạt động độc lập ngay cả khi thiết bị trung tâm gặp sự cố.

---

## PHẦN 4: TỔNG HỢP THÀNH PHẦN AI

| Thành phần | Tự train hay dùng dịch vụ | Công cụ/nguồn | Ghi chú |
|---|---|---|---|
| Wake-word | **Tự train** | Edge Impulse | Chạy trên ESP32-S3, luôn hoạt động |
| Multi-keyword-spotting | **Tự train** | Edge Impulse | Luôn thử trước, bất kể có mạng hay không (local-first) |
| ASR + NLU + hỏi đáp tự do | Dùng dịch vụ | Cloud LLM API (Gemini Flash-Lite / GPT-4.1 mini), function calling | Chỉ gọi khi local không khớp và có mạng |
| Scene tự động gợi ý *(mở rộng)* | Tự viết thuật toán | Association rule mining | Làm sau phần lõi |

---

## PHẦN 5: ĐỘ TIN CẬY HỆ THỐNG

- **Watchdog**: ESP32-S3 giám sát kết nối RCP (H2) qua UART, tự reset nếu mất phản hồi. Mỗi node có FreeRTOS task watchdog riêng.
- **Graceful Degradation**: mất Internet → mất hỏi đáp tự do + lệnh điều kiện phức tạp, còn multi-keyword-spotting cho lệnh cơ bản (không đổi hành vi so với khi có mạng, vì vốn dĩ đã ưu tiên xử lý cục bộ trước); thiết bị trung tâm hỏng hoàn toàn → node vẫn giữ trạng thái, tự động hóa cục bộ (nếu cấu hình) vẫn chạy; 1 node mất kết nối → relay qua node lân cận nhờ tự định tuyến Thread.
- **OTA**: thiết bị trung tâm qua WiFi trực tiếp; node thiết bị qua Thread Mesh (có xác thực/CRC).

---

## PHẦN 6: LỘ TRÌNH THỰC HIỆN (4-5 THÁNG)

| Giai đoạn | Thời gian | Nội dung |
|---|---|---|
| 1. Proof of concept | Tuần 1-2 | Border Router (S3+H2 DevKit) + 1 node đơn giản (bật/tắt LED qua CoAP) |
| 2. Node thiết bị | Tuần 3-6 | Hoàn thiện 4 loại node, thử nghiệm mesh multi-hop, tối ưu năng lượng MTD |
| 3. Voice pipeline | Tuần 7-10 | Wake-word + multi-keyword-spotting, pipeline local-first/cloud-fallback, tích hợp Cloud LLM function calling, TTS |
| 4. Giao diện & kết nối | Tuần 11-13 | Màn hình TFT cảm ứng, MQTT/ThingsBoard, tạo scene/hẹn giờ thủ công |
| 5. Reliability & OTA | Tuần 14-15 | Watchdog, graceful degradation, OTA qua mesh |
| 6. Mở rộng (nếu còn thời gian) | Tuần 16-17 | Scene tự động gợi ý (association rule mining) |
| 7. Thực nghiệm & viết báo cáo | Tuần 18-20 | Đo độ trễ/độ tin cậy mesh, độ chính xác wake-word/keyword-spotting, thời lượng pin, **tỷ lệ xử lý cục bộ thành công (local-hit rate)**, so sánh độ trễ local vs Cloud, hoàn thiện báo cáo |

### Chỉ số thực nghiệm đề xuất cho pipeline giọng nói (chương 4)
- **Local-hit rate**: % lệnh được xử lý thành công hoàn toàn cục bộ trên tổng số lệnh thử nghiệm (test với nhiều người nói, nhiều cách diễn đạt).
- **Độ trễ trung bình**: so sánh xử lý cục bộ vs qua Cloud API (đo từ lúc kết thúc câu nói tới lúc thiết bị phản hồi).
- **Độ chính xác wake-word/keyword-spotting**: tỷ lệ nhận đúng, tỷ lệ báo nhầm (false positive) trong điều kiện có tiếng ồn nền.

---

## PHẦN 7: PHÂN CÔNG NHÓM 2 NGƯỜI

- **Người 1 — Mesh & Firmware thiết bị**: mạng Thread, commissioning, firmware 4 loại node, tối ưu năng lượng MTD, đo thực nghiệm mesh.
- **Người 2 — Thiết bị trung tâm & AI**: firmware ESP32-S3 (audio pipeline local-first/cloud-fallback, giao diện TFT, Border Router, Dispatcher), huấn luyện wake-word + multi-keyword-spotting, tích hợp Cloud LLM function calling, MQTT/ThingsBoard.

---

## PHẦN 8: BOM THAM KHẢO

| Thành phần | Gợi ý | Số lượng |
|---|---|---|
| Host trung tâm | ESP32-S3 DevKit (8MB PSRAM) | 1 |
| RCP | ESP32-H2 DevKit | 1 |
| Màn hình | TFT cảm ứng SPI (ILI9341/ST7789 có touch) | 1 |
| Audio | Mic I2S INMP441, loa + amp I2S MAX98357A | 1 bộ |
| RTC | DS3231 | 1 |
| Node thiết bị (phát triển) | ESP32-H2 DevKit | 4-6 |
| Node thiết bị (hoàn thiện, tùy chọn) | ESP32-H2 SuperMini (nhà bán uy tín) hoặc DevKitM-1 | Theo số node đóng hộp cuối |
| Relay/đo dòng | Relay module/SSR + ACS712 hoặc INA219 | Theo node ổ cắm |
| Dimming đèn | Driver PWM/dimmer AC phù hợp | Theo node đèn |
| Cảm biến môi trường | DHT22 hoặc SHT31 + BH1750 | Theo node cảm biến |
| Motor rèm | Động cơ DC/servo nhỏ + driver + limit switch | Theo node rèm |
| Pin node MTD | Pin Li-Po/AA + mạch quản lý sạc/xả | Theo node cảm biến pin |

---

*Bản cập nhật pipeline giọng nói theo nguyên tắc Local-First/Cloud-Fallback (luôn thử keyword-spotting cục bộ trước, chỉ gọi Cloud khi không khớp), bổ sung chỉ số thực nghiệm local-hit rate và so sánh độ trễ cho chương đánh giá.*
