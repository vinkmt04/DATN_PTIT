#!/usr/bin/env python3
"""
Tool thu âm tập dữ liệu Wake Word TRỰC TIẾP TỪ MIC INMP441 của ESP32 qua Wi-Fi
Chuẩn: 16000 Hz, 16-bit Mono WAV cho Edge Impulse

Cách hoạt động:
1. ESP32 chạy firmware 'dataset_recorder_main.c' và kết nối Wi-Fi cùng mạng với PC.
2. Tool Python này gửi yêu cầu HTTP GET tới ESP32 để kích hoạt thu âm.
3. ESP32 thu 1.0 giây từ mic INMP441 rồi truyền file WAV về máy tính.
4. Tool tự động lưu file vào thư mục dataset tương ứng (hey_cherry, noise, unknown).
"""

import os
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

LABELS = {
    "1": "hey_cherry",
    "2": "noise",
    "3": "unknown"
}

DEFAULT_ESP32_IP = "192.168.10.100"

def get_next_filename(folder: Path, label: str) -> Path:
    existing = list(folder.glob(f"{label}_*.wav"))
    max_idx = 0
    for f in existing:
        try:
            idx = int(f.stem.split("_")[-1])
            if idx > max_idx:
                max_idx = idx
        except ValueError:
            pass
    return folder / f"{label}_{max_idx + 1:04d}.wav"

def test_connection(esp_ip: str) -> bool:
    url = f"http://{esp_ip}/"
    try:
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.status == 200
    except Exception:
        return False

def record_sample_from_esp32(esp_ip: str, label: str, duration: int, output_file: Path, gain: str = "auto") -> bool:
    url = f"http://{esp_ip}/record?label={label}&duration={duration}&gain={gain}"
    try:
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=duration + 10) as resp:
            if resp.status == 200:
                wav_bytes = resp.read()
                with open(output_file, "wb") as f:
                    f.write(wav_bytes)
                return True
    except Exception as e:
        print(f"\n[-] Lỗi kết nối tới ESP32: {e}")
        return False
    return False

def main():
    print("=" * 65)
    print("  TOOL THU ÂM WAKE WORD TRỰC TIẾP TỪ MIC INMP441 (ESP32-S3 WI-FI)")
    print("=" * 65)

    esp_ip = input(f"Nhập IP của ESP32 (Mặc định: {DEFAULT_ESP32_IP}): ").strip()
    if not esp_ip:
        esp_ip = DEFAULT_ESP32_IP

    print(f"[*] Đang kiểm tra kết nối tới ESP32 tại http://{esp_ip}/ ...", end=" ", flush=True)
    if test_connection(esp_ip):
        print("KẾT NỐI TỐT! (ESP32 đã sẵn sàng)")
    else:
        print("CHƯA PHẢN HỒI!")
        print(f"    Hãy chắc chắn ESP32 đã nạp firmware thu âm và máy tính cùng mạng Wi-Fi.")
        choice = input("    Bạn có muốn tiếp tục thử không? (y/n): ").strip().lower()
        if choice != 'y':
            return

    print("\nChọn thời lượng mỗi lần thu:")
    print("  [1] 1.0 giây (Mẫu lẻ lưu ngay)")
    print("  [2] 30 giây  (Nói 'Hey Cherry' ~15 lần để Edge Impulse tự cắt)")
    print("  [3] 60 giây  (Nói 'Hey Cherry' ~30 lần hoặc thu 1 phút tiếng ồn)")
    dur_choice = input("Lựa chọn thời lượng (1/2/3, mặc định 1): ").strip()
    dur_map = {"1": 1, "2": 30, "3": 60}
    duration = dur_map.get(dur_choice, 1)

    print("\nChọn nhãn bạn muốn thu:")
    print("  [1] hey_cherry  (Từ khóa đánh thức: nói rõ 'Hey Cherry' vào mic ESP32)")
    print("  [2] noise       (Tiếng ồn: quạt gió, gõ phím, im lặng phòng quanh ESP32)")
    print("  [3] unknown     (Từ nói tiếng Việt khác: xin chào, thời tiết, ...)")
    print("  [0] Thoát")
    print("-" * 65)

    c = input("Nhập lựa chọn của bạn (1/2/3): ").strip()
    if c not in LABELS:
        print("[-] Thoát.")
        return

    label = LABELS[c]
    save_dir = Path(__file__).parent / "dataset" / label
    save_dir.mkdir(parents=True, exist_ok=True)

    print(f"\n[+] Đang thu nhãn: '{label.upper()}' | Thời lượng: {duration}s")
    print(f"[+] Dữ liệu sẽ lưu vào: {save_dir}")
    if duration > 1:
        print(f"[+] Hướng dẫn: Bấm [ENTER] -> Nói lặp lại 'Hey Cherry' nhiều lần (nghỉ 1.5s giữa các lần).")
        print(f"[+] Sau đó upload file lên Edge Impulse Studio -> Bấm 'Split sample' để tự động cắt!")
    else:
        print("[+] Hướng dẫn: Bấm [ENTER] -> Nói 'Hey Cherry' đúng 1 lần.")
    print("[+] Nhập 'q' rồi [ENTER] bất kỳ lúc nào để dừng lại.\n")

    count = 0
    while True:
        cmd = input(f">> Bấm [ENTER] để thu mẫu tiếp theo (hoặc gõ 'q' để dừng): ")
        if cmd.strip().lower() == 'q':
            break

        print(f"   [Chuẩn bị...] ---> NÓI VÀO MIC ESP32 NGAY! (Thu {duration}s) <---", end="", flush=True)
        out_file = get_next_filename(save_dir, label)
        
        start_t = time.time()
        ok = record_sample_from_esp32(esp_ip, label, duration, out_file)
        
        if ok:
            count += 1
            file_size_kb = out_file.stat().st_size / 1024
            print(f" [XONG!]")
            print(f"   -> Đã lưu: {out_file.name} ({file_size_kb:.1f} KB) (Tổng đã thu: {count} file)")
        else:
            print(" [THẤT BẠI! Vui lòng thử lại]")

    print(f"\n[+] Kết thúc phiên thu! Bạn đã thu được {count} mẫu chuẩn từ mic INMP441.")
    print(f"[+] Dữ liệu sẵn sàng 100% để upload lên Edge Impulse Studio!")

if __name__ == "__main__":
    main()
