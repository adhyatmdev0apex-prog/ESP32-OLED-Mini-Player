#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <Arduino.h>

// ============================================================
// TARGET BOARD SELECTION & PIN CONFIGURATION
// ============================================================
#if defined(TARGET_ESP32CAM) || defined(ARDUINO_ESP32_CAM)
  #define BOARD_TARGET_NAME "ESP32-CAM (with PSRAM)"
#elif defined(TARGET_ESP32DEV_PSRAM)
  #define BOARD_TARGET_NAME "ESP32 Dev Module (with PSRAM)"
#else
  #define BOARD_TARGET_NAME "ESP32 Dev Module (No PSRAM)"
#endif

// --- Display & Physical Button Pin Configuration ---
#define OLED_SDA 13                      // GPIO 13: I2C SDA
#define OLED_SCL 14                      // GPIO 14: I2C SCL
#define PIN_BUTTON_SCROLL 27              // GPIO 27:  Scroll down (Active-LOW to GND)
#define PIN_BUTTON_SELECT 12             // GPIO 12:  Select / Play / Pause (Active-LOW to GND)

// --- Wi-Fi Configuration ---
#if __has_include("secrets.h")
#include "secrets.h"
#elif __has_include("secrets.example.h")
#include "secrets.example.h"
#endif

// Fallbacks if not provided in secrets.h
#ifndef WIFI_SSID
#define WIFI_SSID "YourWiFiSSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YourWiFiPassword"
#endif

#ifndef WIFI_FALLBACK_SSID
#define WIFI_FALLBACK_SSID "YourFallbackSSID"
#endif

#ifndef WIFI_FALLBACK_PASSWORD
#define WIFI_FALLBACK_PASSWORD "YourFallbackPassword"
#endif

// Wi-Fi connection timeout and reconnection intervals (in milliseconds)
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RECONNECT_INTERVAL_MS 9000

// --- Persistent Offline Storage (LittleFS / SPIFFS) ---
#define OFFLINE_STORAGE_LIMIT_BYTES (2050000UL) // 2.05 MB maximum per offline media package
#define OFFLINE_DIR "/offline"
#define OFFLINE_CATALOGUE_PATH "/offline/catalogue.json"
#define LEGACY_OFFLINE_CATALOGUE_PATH "/offline_catalogue.json"

// --- Server & API Configuration ---
#ifndef MEDIA_SERVER_BASE_URL
#define MEDIA_SERVER_BASE_URL "https://oled-mini-player.onrender.com"
#endif

#define HTTP_TIMEOUT_MS 10000
#define DOWNLOAD_TIMEOUT_MS 60000
#define CATALOGUE_PAGE_SIZE 10

// --- Memory & Download Limits ---
#define MAX_VIDEO_SIZE (3500000UL)          // 3.5 MB
#define MAX_AUDIO_SIZE (2500000UL)          // 2.5 MB (supports full-length audio in PSRAM/spiffs)
#define MAX_COMBINED_MEDIA_SIZE (3900000UL) // 3.9 MB (fits in 4.19MB PSRAM)
#define PSRAM_SAFETY_MARGIN_BYTES (250000UL) // 250 KB reserved for stack/heap/BT

// --- OLED Speed ---
#define OLED_I2C_SPEED 800000

// --- Local Fallback Assets ---
#define VIDEO_FILE "/stickman.vs.geometry_dash.bin"
#define AUDIO_FILE "/stickman.vs.geometry_dash.pcm"

// --- Audio Format Configuration ---
#define AUDIO_SAMPLE_RATE 5000 // 5000 Hz, 8-bit unsigned PCM mono
#define BT_AUDIO_SYNC_OFFSET_MS 80 // Pre-lead audio by 80ms to match Bluetooth speaker buffer latency

// --- Physical Button Timing (Active-LOW to GND) ---
#define BUTTON_DEBOUNCE_MS 40             // Debounce period
#define BUTTON_HOLD_SCROLL_INITIAL_MS 450 // Delay before continuous auto-scroll starts
#define BUTTON_HOLD_SCROLL_RATE_MS 200    // Repeat rate for continuous auto-scroll
#define BUTTON_LONG_PRESS_MS 1000         // 1 sec hold triggers Stop & Return to menu during playback
#define BUTTON_HOLD_SAVE_SPIFFS_MS 2000   // 2 sec hold triggers Download to SPIFFS in browse

#endif // APP_CONFIG_H