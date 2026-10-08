#include <Arduino.h>
#include <WiFi.h>
#include "esp_chip_info.h"
#include "esp_flash.h"

// Waveshare ESP32-C6-Pico-M / Zero onboard RGB WS2812 is on GPIO 8
#ifndef RGB_BUILTIN
#define RGB_BUILTIN 8
#endif

// State tracking
static uint32_t loop_count = 0;
static uint32_t last_heartbeat = 0;
static uint8_t color_step = 0;

void set_led_color(uint8_t r, uint8_t g, uint8_t b) {
  // Uses Arduino-ESP32 built-in WS2812 driver
  neopixelWrite(RGB_BUILTIN, r, g, b);
}

void print_system_info() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);

  Serial.println("\n========================================================");
  Serial.println("       WHATTHEFLIP - ESP32-C6 HARDWARE TEST FIRMWARE     ");
  Serial.println("========================================================");
  Serial.printf(" Silicon Model:     ESP32-C6FH4 (Rev %d, 32-bit RISC-V)\n", chip.revision);
  Serial.printf(" CPU Frequency:     %u MHz\n", getCpuFrequencyMhz());
  Serial.printf(" MAC Address:       %s\n", WiFi.macAddress().c_str());
  Serial.printf(" Total Heap:        %u KB\n", ESP.getHeapSize() / 1024);
  Serial.printf(" Free Heap:         %u KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf(" Flash Chip Size:   %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf(" Flash Bus Speed:   %u MHz\n", ESP.getFlashChipSpeed() / 1000000);
  Serial.println("--------------------------------------------------------");
  Serial.println(" Features: Wi-Fi 6 (802.11ax), BLE 5.3, IEEE 802.15.4 (Zigbee/Thread)");
  Serial.println(" RGB LED:  WS2812 on GPIO 8");
  Serial.println("========================================================");
  Serial.println("Commands: 'r' (Red), 'g' (Green), 'b' (Blue), 'w' (White),");
  Serial.println("          's' (Wi-Fi Scan), 'o' (LED Off), 'h' (Help)");
  Serial.println("========================================================\n");
}

void run_wifi_scan() {
  Serial.println("\n[Wi-Fi Scan] Scanning 2.4 GHz channels...");
  set_led_color(64, 0, 64); // Purple during scan
  
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("[Wi-Fi Scan] No networks found.");
  } else {
    Serial.printf("[Wi-Fi Scan] Found %d networks:\n", n);
    for (int i = 0; i < n; ++i) {
      Serial.printf("  [%2d] %-32s | RSSI: %4d dBm | Ch: %2d | Auth: %s\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    WiFi.channel(i),
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "ENCRYPTED");
      delay(10);
    }
  }
  WiFi.scanDelete();
  Serial.println("[Wi-Fi Scan] Scan completed.\n");
}

void setup() {
  // Initialize USB CDC Serial (baud parameter is ignored by native USB, runs at full speed)
  Serial.begin(115200);
  
  // Give host terminal time to attach after USB CDC re-enumeration
  delay(1500);

  // Initialize LED to off
  set_led_color(0, 0, 0);

  // Set Wi-Fi to Station mode for scanning capability
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Print system header
  print_system_info();
}

void loop() {
  // 1. Process serial commands from user / terminal
  if (Serial.available()) {
    char cmd = (char)Serial.read();
    switch (cmd) {
      case 'r':
      case 'R':
        set_led_color(100, 0, 0);
        Serial.println("[CMD] LED -> RED");
        break;
      case 'g':
      case 'G':
        set_led_color(0, 100, 0);
        Serial.println("[CMD] LED -> GREEN");
        break;
      case 'b':
      case 'B':
        set_led_color(0, 0, 100);
        Serial.println("[CMD] LED -> BLUE");
        break;
      case 'w':
      case 'W':
        set_led_color(60, 60, 60);
        Serial.println("[CMD] LED -> WHITE");
        break;
      case 'o':
      case 'O':
        set_led_color(0, 0, 0);
        Serial.println("[CMD] LED -> OFF");
        break;
      case 's':
      case 'S':
        run_wifi_scan();
        break;
      case 'h':
      case 'H':
      case '?':
        print_system_info();
        break;
      default:
        // Ignore whitespace / newlines
        break;
    }
  }

  // 2. Periodic color cycle and heartbeat log every 1000ms
  uint32_t now = millis();
  if (now - last_heartbeat >= 1000) {
    last_heartbeat = now;
    loop_count++;

    // Cycle through 6 colors: Red, Yellow, Green, Cyan, Blue, Magenta
    switch (color_step % 6) {
      case 0: set_led_color(40, 0, 0);   break; // Red
      case 1: set_led_color(30, 30, 0);  break; // Yellow
      case 2: set_led_color(0, 40, 0);   break; // Green
      case 3: set_led_color(0, 30, 30);  break; // Cyan
      case 4: set_led_color(0, 0, 40);   break; // Blue
      case 5: set_led_color(30, 0, 30);  break; // Magenta
    }
    color_step++;

    // Print heartbeat telemetry
    Serial.printf("[HEARTBEAT #%u] Uptime: %lu s | Free Heap: %u bytes (%u KB) | LED Step: %u/6\n",
                  loop_count,
                  now / 1000,
                  ESP.getFreeHeap(),
                  ESP.getFreeHeap() / 1024,
                  (color_step % 6) + 1);
  }

  delay(20);
}
