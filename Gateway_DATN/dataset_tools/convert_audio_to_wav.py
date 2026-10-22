#!/usr/bin/env python3
"""
Tool chuyển đổi định dạng âm thanh (.m4a, .mp3, .aac, .wav) sang chuẩn Edge Impulse:
- Tần số lấy mẫu: 16000 Hz (16 kHz)
- Số kênh: Mono (1 kênh)
- Độ sâu bit: 16-bit PCM Signed Integer (.wav)

Hướng dẫn cài đặt thư viện hỗ trợ (nếu chưa có):
    pip install pydub
    (và tải ffmpeg nếu dùng Windows: winget install Gyan.FFmpeg hoặc tải ffmpeg.exe)
"""

import os
import sys
import subprocess
from pathlib import Path

def get_ffmpeg_executable():
    """Tìm đường dẫn ffmpeg khả dụng (từ imageio_ffmpeg hoặc system PATH)"""
    # 1. Thử dùng imageio_ffmpeg (tự động có nếu pip install imageio-ffmpeg)
    try:
        import imageio_ffmpeg
        exe = imageio_ffmpeg.get_ffmpeg_exe()
        if exe and os.path.exists(exe):
            return exe
    except ImportError:
        pass

    # 2. Thử ffmpeg trong biến môi trường PATH
    import shutil
    system_ffmpeg = shutil.which("ffmpeg")
    if system_ffmpeg:
        return system_ffmpeg

    return None

def convert_with_ffmpeg(input_path, output_path, ffmpeg_exe="ffmpeg"):
    """Chuyển đổi trực tiếp bằng ffmpeg command-line"""
    cmd = [
        str(ffmpeg_exe), "-y",
        "-i", str(input_path),
        "-ar", "16000",
        "-ac", "1",
        "-c:a", "pcm_s16le",
        str(output_path)
    ]
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return result.returncode == 0
    except Exception:
        return False

def convert_with_pydub(input_path, output_path):
    """Chuyển đổi bằng thư viện pydub trong Python"""
    try:
        from pydub import AudioSegment
        sound = AudioSegment.from_file(str(input_path))
        sound = sound.set_frame_rate(16000).set_channels(1).set_sample_width(2)
        sound.export(str(output_path), format="wav")
        return True
    except Exception:
        return False

def convert_audio(input_file, output_file, ffmpeg_exe=None):
    # Ưu tiên dùng ffmpeg_exe nếu tìm thấy
    if ffmpeg_exe and convert_with_ffmpeg(input_file, output_file, ffmpeg_exe):
        return True
    if convert_with_ffmpeg(input_file, output_file, "ffmpeg"):
        return True
    if convert_with_pydub(input_file, output_file):
        return True
    return False

def batch_convert(input_dir, output_dir):
    input_path = Path(input_dir)
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    supported_exts = {".m4a", ".mp3", ".aac", ".ogg", ".flac", ".wav", ".wma"}
    files = [f for f in input_path.iterdir() if f.suffix.lower() in supported_exts]

    if not files:
        print(f"[-] Không tìm thấy file âm thanh nào trong: {input_dir}")
        print(f"    Hãy copy các file .m4a từ điện thoại vào thư mục này!")
        return

    ffmpeg_exe = get_ffmpeg_executable()
    if ffmpeg_exe:
        print(f"[+] Tìm thấy bộ giải mã FFmpeg tại: {ffmpeg_exe}")
    else:
        print("[!] CẢNH BÁO: Chưa tìm thấy FFmpeg.")
        print("    -> Giải pháp nhanh nhất: Mở terminal chạy lệnh: pip install imageio-ffmpeg\n")

    print(f"[*] Tìm thấy {len(files)} file cần chuyển đổi trong '{input_dir}'...")
    success_count = 0

    for i, file in enumerate(files, 1):
        out_name = f"{file.stem}.wav"
        out_file = output_path / out_name
        print(f"[{i}/{len(files)}] Đang chuyển đổi: {file.name} -> {out_name} ...", end=" ")
        
        if convert_audio(file, out_file, ffmpeg_exe):
            print("OK")
            success_count += 1
        else:
            print("LỖI! (Hãy chạy: pip install imageio-ffmpeg)")

    print(f"\n[+] Hoàn thành! Đã chuyển đổi thành công {success_count}/{len(files)} file vào '{output_dir}'.")
    if success_count > 0:
        print(f"[+] Các file này đã sẵn sàng 100% để upload lên Edge Impulse Studio!")

if __name__ == "__main__":
    current_dir = Path(__file__).parent
    input_folder = current_dir / "raw_m4a"
    output_folder = current_dir / "converted_wav"

    input_folder.mkdir(parents=True, exist_ok=True)
    output_folder.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print("TOOL CHUYỂN ĐỔI FILE .M4A TỪ ĐIỆN THOẠI SANG WAV 16KHZ MONO")
    print("=" * 60)
    print(f"1. Thư mục chứa file gốc (.m4a, .mp3 từ bạn bè): {input_folder}")
    print(f"2. Thư mục xuất file chuẩn (.wav 16kHz):        {output_folder}")
    print("-" * 60)

    batch_convert(input_folder, output_folder)
