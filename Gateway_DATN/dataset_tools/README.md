# Hướng Dẫn Thu Thập & Xử Lý Dữ Liệu Wake Word "Hey Cherry"

Thư mục này chứa 3 công cụ giúp bạn xây dựng bộ dữ liệu âm thanh chuẩn cho Edge Impulse:
1. `record_esp32_wifi.py`: Thu âm **trực tiếp từ mic phần cứng INMP441 của ESP32 qua Wi-Fi** (Khuyên dùng để lấy chất âm thật của mạch).
2. `record_pc.py`: Tool thu âm nhanh 1 giây trên máy tính bằng microphone laptop/PC.
3. `convert_audio_to_wav.py`: Tool chuyển đổi hàng loạt file `.m4a` từ điện thoại sang `.wav` 16kHz Mono.

---

## 1. Cài Đặt Môi Trường Python

Chạy lệnh cài các thư viện cần thiết:
```powershell
pip install sounddevice numpy scipy pydub imageio-ffmpeg
```
*(Gói `imageio-ffmpeg` giúp giải nén file `.m4a` từ điện thoại một cách tự động mà không cần cài thêm phần mềm ngoài).*

---

## 2. PHƯƠNG PHÁP 1: Thu Âm Trực Tiếp Từ Mic INMP441 Trên ESP32 (Chuẩn Phần Cứng)

Đây là phương pháp quan trọng nhất để AI học đúng chất âm, dải tần và độ nhạy của mic INMP441.

### Bước 1: Nạp firmware thu âm vào ESP32
1. Mở file [main/CMakeLists.txt](file:///d:/PTIT/DATN_PTIT/i2s_example/i2s_recorder_test/main/CMakeLists.txt):
   - Đổi dòng:
     ```cmake
     set(ACTIVE_APP "dataset_recorder_main.c")
     # set(ACTIVE_APP "main.c")
     ```
2. Nạp code vào ESP32:
   ```powershell
   idf.py build flash monitor
   ```
3. ESP32 sẽ kết nối Wi-Fi và hiển thị địa chỉ IP (ví dụ: `192.168.10.100`).

### Bước 2: Thu âm
Bạn có thể chọn 1 trong 2 cách:
* **Cách A: Mở trình duyệt Web (trên điện thoại hoặc laptop)**:
  - Truy cập: `http://192.168.10.100` (thay bằng IP thật của ESP32).
  - Web có giao diện bấm nút: `[Thu 'Hey Cherry']`, `[Thu 'Noise']`, `[Thu 'Unknown']`.
  - Bấm nút -> Nói vào mic ESP32 -> Web phát lại âm thanh vừa thu và tự động tải file `.wav` về máy!
* **Cách B: Dùng tool Python tự động lưu vào thư mục dataset**:
  - Chạy lệnh:
    ```powershell
    python dataset_tools/record_esp32_wifi.py
    ```
  - Nhập IP ESP32, chọn nhãn, sau đó chỉ cần bấm `[ENTER]` liên tục để thu từng mẫu. File `.wav` sẽ tự động lưu vào `dataset_tools/dataset/<label>/`.

---

## 3. PHƯƠNG PHÁP 2: Tự Thu Bằng Mic Máy Tính (`record_pc.py`)

Chạy lệnh:
```powershell
python dataset_tools/record_pc.py
```
* Bấm `[ENTER]` -> Màn hình đếm ngược `3.. 2.. 1.. NÓI NGAY!` -> Bạn nói *"Hey Cherry"*.
* Mẫu dài đúng 1.0 giây và được tự động lưu vào `dataset_tools/dataset/<label>/`.

---

## 4. PHƯƠNG PHÁP 3: Chuyển Đổi File Từ Điện Thoại Bạn Bè (`convert_audio_to_wav.py`)

Khi nhờ bạn bè (nam, nữ) thu âm bằng app Ghi âm trên iPhone / Android:
1. Bảo họ nói câu *"Hey Cherry"* lặp đi lặp lại khoảng 10 - 15 lần trong 1 file ghi âm.
2. Copy các file `.m4a`, `.mp3` vào thư mục:
   `dataset_tools/raw_m4a/`
3. Chạy tool chuyển đổi:
   ```powershell
   python dataset_tools/convert_audio_to_wav.py
   ```
4. File xuất chuẩn 16kHz Mono sẽ nằm trong:
   `dataset_tools/converted_wav/`

---

## 5. Tải Lên Edge Impulse Studio

1. Truy cập [studio.edgeimpulse.com](https://studio.edgeimpulse.com/) -> Đăng nhập -> Tạo Project mới (**Audio**).
2. Vào **Data acquisition** -> Chọn tab **Upload data**.
3. Chọn các file `.wav` trong thư mục `dataset/` hoặc `converted_wav/`.
4. Điền nhãn tương ứng (**Enter label**): `hey_cherry`, `noise`, hoặc `unknown`.
5. Tỉ lệ chia Train/Test: Chọn **Automatically split between train and test** (80% Train / 20% Test).
6. Bấm **Upload data**.
