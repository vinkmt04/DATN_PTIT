/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* I2S Digital Microphone Recording Example */
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "format_wav.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "i2s_recorder";

#define NUM_CHANNELS (1) // For mono recording only!
#define SAMPLE_SIZE (CONFIG_EXAMPLE_BIT_SAMPLE * 1024)
#define BYTE_RATE                                                              \
  (CONFIG_EXAMPLE_SAMPLE_RATE * (CONFIG_EXAMPLE_BIT_SAMPLE / 8)) * NUM_CHANNELS

i2s_chan_handle_t rx_handle = NULL;
i2s_chan_handle_t tx_handle = NULL;
uint8_t *audio_file_buffer = NULL;
uint32_t audio_file_size = 0;

static int16_t i2s_readraw_buff[SAMPLE_SIZE];
size_t bytes_read;
const int WAVE_HEADER_SIZE = 44;

#define MAX_RECORD_SIZE (4 * 1024 * 1024) // Tối đa 4MB

volatile bool is_recording = false;
volatile bool stop_recording_req = false;
volatile bool play_req = false;

void console_task(void *pvParameters) {
  while (1) {
    int c = fgetc(stdin);
    if (c != EOF) {
      if (c == '1') {
        if (!is_recording) {
          is_recording = true;
        } else {
          stop_recording_req = true;
        }
      } else if (c == '2') {
        if (!is_recording) {
          play_req = true;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void record_wav_to_psram_until_stopped(void) {
  ESP_LOGI(TAG, "Bắt đầu ghi âm... Nhấn '1' lần nữa để dừng.");

  if (audio_file_buffer == NULL) {
    audio_file_buffer = (uint8_t *)heap_caps_malloc(MAX_RECORD_SIZE, MALLOC_CAP_SPIRAM);
    if (audio_file_buffer == NULL) {
      ESP_LOGE(TAG, "Không thể cấp phát %d bytes trên PSRAM", MAX_RECORD_SIZE);
      return;
    }
  }

  uint32_t current_offset = WAVE_HEADER_SIZE;
  stop_recording_req = false;

  while (!stop_recording_req) {
    if (current_offset + SAMPLE_SIZE > MAX_RECORD_SIZE) {
      ESP_LOGW(TAG, "Bộ nhớ đầy, tự động dừng ghi âm!");
      break;
    }

    if (i2s_channel_read(rx_handle, (char *)i2s_readraw_buff, SAMPLE_SIZE,
                         &bytes_read, 1000) == ESP_OK) {
      memcpy(audio_file_buffer + current_offset, i2s_readraw_buff, bytes_read);
      current_offset += bytes_read;
    } else {
      ESP_LOGE(TAG, "Lỗi đọc I2S!");
    }
  }

  audio_file_size = current_offset;

  // Cập nhật lại header WAV với kích thước thật
  uint32_t flash_rec_time = audio_file_size - WAVE_HEADER_SIZE;
  const wav_header_t wav_header =
      WAV_HEADER_PCM_DEFAULT(flash_rec_time, 16, CONFIG_EXAMPLE_SAMPLE_RATE, 1);
  memcpy(audio_file_buffer, &wav_header, WAVE_HEADER_SIZE);

  ESP_LOGI(TAG, "Ghi âm kết thúc! Đã lưu %lu bytes", audio_file_size);
}

void play_audio_from_psram(void) {
  if (audio_file_buffer == NULL || audio_file_size <= WAVE_HEADER_SIZE) {
    ESP_LOGE(TAG, "No audio data to play!");
    return;
  }

  ESP_LOGI(TAG, "Bắt đầu phát lại âm thanh ra loa MAX98357...");

  size_t bytes_written = 0;
  uint32_t current_offset = WAVE_HEADER_SIZE; // Bỏ qua 44 byte Header

  // Phát từng khối nhỏ (4096 bytes) để tránh nghẽn CPU
  const uint32_t chunk_size = 4096;

  while (current_offset < audio_file_size) {
    uint32_t remaining = audio_file_size - current_offset;
    uint32_t write_size = (remaining < chunk_size) ? remaining : chunk_size;

    if (i2s_channel_write(tx_handle, audio_file_buffer + current_offset,
                          write_size, &bytes_written, 1000) == ESP_OK) {

      // In ra 4 giá trị mẫu đang đẩy ra loa
      // int16_t *samples = (int16_t *)(audio_file_buffer + current_offset);
      // ESP_LOGI(TAG, "[Speaker] Phát %d bytes. Mẫu: %d, %d, %d, %d...",
      //          bytes_written, samples[0], samples[1], samples[2],
      //          samples[3]);

      current_offset += bytes_written;
    } else {
      ESP_LOGE(TAG, "Lỗi khi phát audio!");
      break;
    }
  }

  ESP_LOGI(TAG, "Đã phát xong!");
}

void init_microphone(void) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_EXAMPLE_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = 4, // Đã chọn GPIO 4 cho chân SCK của INMP441
              .ws = 5,   // Đã chọn GPIO 5 cho chân WS (L/R) của INMP441
              .dout = I2S_GPIO_UNUSED,
              .din = 6, // Đã chọn GPIO 6 cho chân SD (Data) của INMP441
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  // Thiết lập kênh MONO lấy dữ liệu ở khe trái (Left Slot)
  // Nếu INMP441 của bạn nối chân L/R xuống GND, nó sẽ xuất data ở Left Slot.
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  // Khởi tạo kênh theo chuẩn STD (Standard) thay vì PDM
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

void init_speaker(void) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_EXAMPLE_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = 15, // Đã chọn GPIO 15 cho chân BCLK của MAX98357
              .ws = 16,   // Đã chọn GPIO 16 cho chân LRC của MAX98357
              .dout = 17, // Đã chọn GPIO 17 cho chân DIN của MAX98357
              .din = I2S_GPIO_UNUSED,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
}

void app_main(void) {
  printf("--------------------------------------\n");
  printf("HƯỚNG DẪN SỬ DỤNG:\n");
  printf("- Ấn '1' để bắt đầu ghi âm.\n");
  printf("- Ấn '1' lần nữa để dừng ghi âm.\n");
  printf("- Ấn '2' để phát lại đoạn ghi âm.\n");
  printf("--------------------------------------\n");

  // Khởi tạo Micro và Loa
  init_microphone();
  init_speaker();

  // Tạo Task để đọc phím từ Serial Monitor
  xTaskCreate(console_task, "console_task", 4096, NULL, 5, NULL);

  while (1) {
    if (is_recording) {
      record_wav_to_psram_until_stopped();
      is_recording = false;
    } else if (play_req) {
      play_req = false;
      play_audio_from_psram();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
