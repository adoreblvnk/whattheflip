#include <Arduino.h>
#include <LittleFS.h>
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n\n========================================================");
  Serial.println("   ESP32-C6 HARDWARE BENCHMARK & STORAGE ASSESSMENT     ");
  Serial.println("========================================================");

  // 1. CPU & CHIP INFO
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  Serial.printf("[1] SILICON: ESP32-C6FH4 Revision %d (Single-Core RV32IMAC)\n", chip_info.revision);
  Serial.printf("    CPU Clock Frequency:    %u MHz\n", getCpuFrequencyMhz());
  Serial.printf("    Crystal XTAL Frequency: %u MHz\n", getXtalFrequencyMhz());
  Serial.printf("    Total Internal SRAM:    %u KB\n", ESP.getHeapSize() / 1024);
  Serial.printf("    Free Heap Available:    %u KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("    Max Contiguous Alloc:   %u KB\n", ESP.getMaxAllocHeap() / 1024);

  // 2. EMBEDDED FLASH METRICS
  uint32_t flash_size = ESP.getFlashChipSize();
  uint32_t flash_speed = ESP.getFlashChipSpeed();
  Serial.println("\n[2] EMBEDDED FLASH METRICS:");
  Serial.printf("    Physical Flash Size:    %u bytes (%u MB)\n", flash_size, flash_size / (1024 * 1024));
  Serial.printf("    Flash Bus Clock:        %u MHz\n", flash_speed / 1000000);
  Serial.printf("    Flash Mode:             %d (QIO/DIO)\n", ESP.getFlashChipMode());

  // 3. LITTLEFS REAL-WORLD PERFORMANCE (CAN IT REPLACE MICROSD?)
  Serial.println("\n[3] INTERNAL FLASH (LittleFS) THROUGHPUT BENCHMARK:");
  if (!LittleFS.begin(true)) {
    Serial.println("    FAILED: LittleFS mount error!");
  } else {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    size_t free_space = total - used;
    Serial.printf("    LittleFS Total Size:    %u bytes (%u KB / %.2f MB)\n", 
                  total, total / 1024, (float)total / (1024.0 * 1024.0));
    Serial.printf("    LittleFS Free Space:    %u bytes (%u KB / %.2f MB)\n", 
                  free_space, free_space / 1024, (float)free_space / (1024.0 * 1024.0));

    // Measure Write Throughput (Writing 128 KB simulated PCAP stream in 512B chunks)
    const size_t test_size = 128 * 1024; // 128 KB
    const size_t chunk_size = 512;
    uint8_t dummy_chunk[chunk_size];
    for (size_t i = 0; i < chunk_size; i++) dummy_chunk[i] = (uint8_t)(i & 0xFF);

    File f = LittleFS.open("/stream_test.pcap", "w");
    if (f) {
      uint32_t t_start = millis();
      size_t written = 0;
      while (written < test_size) {
        written += f.write(dummy_chunk, chunk_size);
      }
      f.flush();
      f.close();
      uint32_t t_write = millis() - t_start;
      float write_speed = (float)test_size / ((float)t_write / 1000.0) / 1024.0; // KB/s

      Serial.printf("    Write Test (128 KB):    %u ms elapsed -> Throughput: %.2f KB/s (%.2f kbps)\n", 
                    t_write, write_speed, write_speed * 8.0);

      // Measure Read Throughput
      f = LittleFS.open("/stream_test.pcap", "r");
      if (f) {
        t_start = millis();
        size_t bytes_read = 0;
        while (f.available()) {
          bytes_read += f.read(dummy_chunk, chunk_size);
        }
        f.close();
        uint32_t t_read = millis() - t_start;
        float read_speed = (float)bytes_read / ((float)t_read / 1000.0) / 1024.0; // KB/s

        Serial.printf("    Read Test (128 KB):     %u ms elapsed -> Throughput: %.2f KB/s (%.2f kbps)\n", 
                      t_read, read_speed, read_speed * 8.0);
      }
      LittleFS.remove("/stream_test.pcap");
    } else {
      Serial.println("    FAILED: Could not open test file for writing!");
    }
  }

  Serial.println("\n========================================================");
  Serial.println("   BENCHMARK RUN FINISHED - ENTERING LOOP HEARTS       ");
  Serial.println("========================================================");
}

void loop() {
  static uint32_t count = 0;
  count++;
  Serial.printf("[Heartbeat #%u] System Alive | Uptime: %lu ms | Free Heap: %u KB\n",
                count, millis(), ESP.getFreeHeap() / 1024);
  delay(2000);
}
