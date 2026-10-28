// secrets.example.h
// This file is a template for storing sensitive information such as Wi-Fi credentials.
// Copy this file to "secrets.h" in the src/config/ directory and fill in your actual credentials. Do not commit "secrets.h" to version control.

// --- Wi-Fi Configuration ---
#ifndef WIFI_SSID
#define WIFI_SSID "YourWiFiSSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YourWiFiPassword"
#endif

// Secondary / Fallback Wi-Fi
#ifndef WIFI_FALLBACK_SSID
#define WIFI_FALLBACK_SSID "YourFallbackSSID"
#endif

#ifndef WIFI_FALLBACK_PASSWORD
#define WIFI_FALLBACK_PASSWORD "YourFallbackPassword"
#endif
