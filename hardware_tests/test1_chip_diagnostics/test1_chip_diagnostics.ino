#include <Arduino.h>
#include <LittleFS.h>
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

void run_diagnostics() {
  Serial.println("==================================================");
  Serial.println("  ESP32-C6 HARDWARE CAPABILITY & STORAGE TEST     ");
  Serial.println("==================================================");

  // 1. Chip and CPU Info
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  Serial.printf("Model: ESP32-C6 (Cores: %d, Rev: %d)\n", chip_info.cores, chip_info.revision);
  Serial.printf("CPU Frequency: %u MHz\n", getCpuFrequencyMhz());
  Serial.printf("XTAL Frequency: %u MHz\n", getXtalFrequencyMhz());

  // 2. Memory (SRAM / Heap) Analysis
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = ESP.getHeapSize();
  uint32_t maxAlloc = ESP.getMaxAllocHeap();
  uint32_t minFreeHeap = ESP.getMinFreeHeap();

  Serial.println("\n[SRAM & HEAP METRICS]");
  Serial.printf("Total Heap:      %u bytes (%u KB)\n", totalHeap, totalHeap / 1024);
  Serial.printf("Free Heap:       %u bytes (%u KB)\n", freeHeap, freeHeap / 1024);
  Serial.printf("Max Alloc Block: %u bytes (%u KB)\n", maxAlloc, maxAlloc / 1024);
  Serial.printf("Min Free Heap:   %u bytes (%u KB)\n", minFreeHeap, minFreeHeap / 1024);

  // 3. Flash Memory Metrics
  uint32_t flashSize = ESP.getFlashChipSize();
  uint32_t flashSpeed = ESP.getFlashChipSpeed();
  FlashMode_t flashMode = ESP.getFlashChipMode();

  Serial.println("\n[EMBEDDED FLASH METRICS]");
  Serial.printf("Flash Chip Size:  %u bytes (%u MB)\n", flashSize, flashSize / (1024 * 1024));
  Serial.printf("Flash Chip Speed: %u MHz\n", flashSpeed / 1000000);
  Serial.printf("Flash Mode:       %d (0:QIO, 1:QOUT, 2:DIO, 3:DOUT)\n", flashMode);

  // 4. LittleFS Internal Storage Benchmark (Replacing MicroSD Test)
  Serial.println("\n[LITTLEFS INTERNAL STORAGE BENCHMARK]");
  if (!LittleFS.begin(true)) {
    Serial.println("FAILED: LittleFS mount/format failed!");
  } else {
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    Serial.printf("LittleFS Total Space: %u bytes (%u KB / %.2f MB)\n", 
                  totalBytes, totalBytes / 1024, (float)totalBytes / (1024.0 * 1024.0));
    Serial.printf("LittleFS Used Space:  %u bytes (%u KB)\n", usedBytes, usedBytes / 1024);
    Serial.printf("LittleFS Free Space:  %u bytes (%u KB / %.2f MB)\n", 
                  totalBytes - usedBytes, (totalBytes - usedBytes) / 1024, (float)(totalBytes - usedBytes) / (1024.0 * 1024.0));

    // Test Write Speed with a simulated PCAP buffer (64 KB chunk write)
    const size_t testSize = 64 * 1024; // 64 KB
    const size_t chunkSize = 512;      // typical packet buffer
    uint8_t dummyPacket[chunkSize];
    memset(dummyPacket, 0xAA, chunkSize);

    File testFile = LittleFS.open("/test_capture.pcap", "w");
    if (testFile) {
      uint32_t startWrite = millis();
      size_t written = 0;
      while (written < testSize) {
        written += testFile.write(dummyPacket, chunkSize);
      }
      testFile.flush();
      testFile.close();
      uint32_t writeTime = millis() - startWrite;
      float writeSpeedKBps = (float)testSize / (float)writeTime; // KB/s

      Serial.printf("Write Test: %u bytes written in %u ms (Throughput: %.2f KB/s)\n", 
                    testSize, writeTime, writeSpeedKBps);

      // Test Read Speed
      testFile = LittleFS.open("/test_capture.pcap", "r");
      if (testFile) {
        uint32_t startRead = millis();
        size_t bytesRead = 0;
        while (testFile.available()) {
          bytesRead += testFile.read(dummyPacket, chunkSize);
        }
        testFile.close();
        uint32_t readTime = millis() - startRead;
        float readSpeedKBps = (float)bytesRead / (float)readTime;

        Serial.printf("Read Test:  %u bytes read in %u ms (Throughput: %.2f KB/s)\n", 
                      bytesRead, readTime, readSpeedKBps);
      }
      LittleFS.remove("/test_capture.pcap");
    } else {
      Serial.println("FAILED to open /test_capture.pcap for writing");
    }
  }

  Serial.println("\n==================================================");
  Serial.println("  TEST 1 COMPLETED                                ");
  Serial.println("==================================================");
}

void setup() {
  Serial.begin(115200);
}

void loop() {
  run_diagnostics();
  delay(3000);
}
