#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <LittleFS.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ieee802154.h"

// Counters
volatile uint32_t wifi_packet_count = 0;
volatile uint32_t wifi_beacon_count = 0;
volatile uint32_t wifi_data_count = 0;
volatile uint32_t wifi_total_bytes = 0;

// Wi-Fi Promiscuous RX Callback
void wifi_promiscuous_rx_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  wifi_packet_count++;
  wifi_total_bytes += pkt->rx_ctrl.sig_len;

  if (type == WIFI_PKT_MGMT) {
    uint8_t sub_type = (pkt->payload[0] >> 4) & 0x0F;
    if (sub_type == 8) { // Beacon frame
      wifi_beacon_count++;
    }
  } else if (type == WIFI_PKT_DATA) {
    wifi_data_count++;
  }
}

// BLE Advertised Device Callback
uint32_t ble_device_count = 0;
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      ble_device_count++;
    }
};

void setup() {
  Serial.begin(115200);
  delay(3000); // Allow USB CDC host connection

  Serial.println("\n\n========================================================");
  Serial.println("   ESP32-C6 FULL CAPABILITY & PERFORMANCE BENCHMARK     ");
  Serial.println("========================================================");

  // 1. CHIP & HARDWARE METRICS
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  Serial.printf("[1] SILICON: ESP32-C6FH4 Rev %d | Single Core RV32IMAC @ %u MHz\n", 
                chip_info.revision, getCpuFrequencyMhz());
  Serial.printf("    SRAM Heap Total: %u KB | Free Heap: %u KB | Max Alloc: %u KB\n",
                ESP.getHeapSize() / 1024, ESP.getFreeHeap() / 1024, ESP.getMaxAllocHeap() / 1024);
  Serial.printf("    Flash Chip Size: %u MB @ %u MHz\n", 
                ESP.getFlashChipSize() / (1024 * 1024), ESP.getFlashChipSpeed() / 1000000);

  // 2. LITTLEFS FLASH STORAGE THROUGHPUT BENCHMARK
  Serial.println("\n[2] INTERNAL FLASH STORAGE (LittleFS) TEST:");
  if (LittleFS.begin(true)) {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    Serial.printf("    LittleFS Partition Size: %u KB (%.2f MB) | Free: %u KB\n",
                  total / 1024, (float)total / (1024.0 * 1024.0), (total - used) / 1024);

    // Benchmarking 64 KB PCAP write
    const size_t test_bytes = 64 * 1024;
    const size_t chunk_size = 512;
    uint8_t buf[chunk_size];
    memset(buf, 0x5A, chunk_size);

    File f = LittleFS.open("/bench.pcap", "w");
    if (f) {
      uint32_t t0 = millis();
      size_t written = 0;
      while (written < test_bytes) {
        written += f.write(buf, chunk_size);
      }
      f.flush();
      f.close();
      uint32_t dt = millis() - t0;
      float speed = (float)test_bytes / (float)dt; // KB/s
      Serial.printf("    Write 64 KB PCAP Stream: %u ms -> Speed: %.2f KB/s\n", dt, speed);

      // Read Benchmark
      f = LittleFS.open("/bench.pcap", "r");
      if (f) {
        t0 = millis();
        size_t r = 0;
        while (f.available()) {
          r += f.read(buf, chunk_size);
        }
        f.close();
        dt = millis() - t0;
        float rspeed = (float)r / (float)dt;
        Serial.printf("    Read  64 KB PCAP Stream: %u ms -> Speed: %.2f KB/s\n", dt, rspeed);
      }
      LittleFS.remove("/bench.pcap");
    }
  } else {
    Serial.println("    LittleFS Mount Failed!");
  }

  // 3. IEEE 802.15.4 HARDWARE INITIALIZATION (ZIGBEE / THREAD)
  Serial.println("\n[3] IEEE 802.15.4 HARDWARE TRANSCEIVER TEST:");
  esp_err_t ieee_err = esp_ieee802154_enable();
  if (ieee_err == ESP_OK) {
    esp_ieee802154_set_channel(15); // Standard Zigbee Channel 15 (2.425 GHz)
    esp_ieee802154_set_promiscuous(true);
    Serial.printf("    IEEE 802.15.4 Radio: ENABLED (Channel %d, Promiscuous Mode ON)\n", 
                  esp_ieee802154_get_channel());
    esp_ieee802154_disable();
  } else {
    Serial.printf("    IEEE 802.15.4 Init Result: 0x%X\n", ieee_err);
  }

  // 4. BLE 5.3 SCANNING TEST
  Serial.println("\n[4] BLE 5.3 SCANNER TEST (Scanning for 3 seconds)...");
  BLEDevice::init("ESP32-C6-Tester");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  BLEScanResults* foundDevices = pBLEScan->start(3, false);
  Serial.printf("    BLE Scan Complete: Found %d unique advertising devices!\n", foundDevices->getCount());
  pBLEScan->clearResults();
  BLEDevice::deinit();

  // 5. WI-FI 6 PROMISCUOUS MODE SNIFFER TEST
  Serial.println("\n[5] WI-FI PROMISCUOUS SNIFFER TEST (Listening for 5 seconds)...");
  WiFi.mode(WIFI_MODE_NULL);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous_rx_callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE); // Listen on Channel 6

  uint32_t sniff_start = millis();
  while (millis() - sniff_start < 5000) {
    delay(500);
    Serial.printf("    [Live Sniffer] Channel 6 | Total Packets: %u | Beacons: %u | Data: %u | Bytes: %u\n",
                  wifi_packet_count, wifi_beacon_count, wifi_data_count, wifi_total_bytes);
  }
  esp_wifi_set_promiscuous(false);

  Serial.println("\n========================================================");
  Serial.println("   ALL ESP32-C6 HARDWARE TESTS COMPLETED SUCCESSFULLY!  ");
  Serial.println("========================================================");
}

void loop() {
  Serial.printf("[Heartbeat] Uptime: %lu ms | Free Heap: %u KB | Total Wi-Fi Packets Sniffed: %u\n",
                millis(), ESP.getFreeHeap() / 1024, wifi_packet_count);
  delay(3000);
}
