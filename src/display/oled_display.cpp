#include "oled_display.h"

OLEDDisplay& OLEDDisplay::getInstance() {
    static OLEDDisplay instance;
    return instance;
}

OLEDDisplay::OLEDDisplay()
    : m_u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA)
    , m_displayConnected(false) {
}

void OLEDDisplay::begin() {
    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(OLED_I2C_SPEED);
    Wire.setTimeOut(10); // 10ms quick probe timeout

    Wire.beginTransmission(0x3C);
    uint8_t err = Wire.endTransmission();
    m_displayConnected = (err == 0);

    if (m_displayConnected) {
        Serial.println("[DISPLAY] SSD1306 OLED detected on I2C (0x3C).");
        m_u8g2.setBusClock(OLED_I2C_SPEED);
        m_u8g2.begin();
        Wire.setClock(OLED_I2C_SPEED);
        m_u8g2.clearBuffer();
        m_u8g2.sendBuffer();
    } else {
        Serial.printf("[DISPLAY] No OLED display detected on I2C (SDA=%d, SCL=%d). Headless mode active.\n",
                      OLED_SDA, OLED_SCL);
    }
}

void OLEDDisplay::clear() {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    m_u8g2.sendBuffer();
}

void OLEDDisplay::showSplashScreen() {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    m_u8g2.setFont(u8g2_font_6x10_tf);
    m_u8g2.drawStr(10, 15, "ESP32-CAM MEDIA");
    m_u8g2.drawStr(15, 30, "STIK + BT PLAYER");
    m_u8g2.drawStr(5, 55, "Connecting Wi-Fi...");
    m_u8g2.sendBuffer();
}

void OLEDDisplay::showStatusScreen(const char* line1, const char* line2, const char* line3) {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    m_u8g2.setFont(u8g2_font_6x10_tf);
    if (line1) m_u8g2.drawStr(0, 15, line1);
    if (line2) m_u8g2.drawStr(0, 35, line2);
    if (line3) m_u8g2.drawStr(0, 55, line3);
    m_u8g2.sendBuffer();
}

void OLEDDisplay::showCatalogueScreen(size_t currentIndex, size_t totalItems, const char* title, const char* id,
                                      bool isOfflineCompatible, bool isOfflineStored, bool isOfflineMode) {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();

    // Top Header Banner: inverted bar
    m_u8g2.drawBox(0, 0, 128, 12);
    m_u8g2.setFont(u8g2_font_5x8_tf);
    m_u8g2.setDrawColor(0); // Inverted black text on white
    char header[32];
    if (isOfflineMode) {
        snprintf(header, sizeof(header), "OFFLINE LIB %u/%u", (unsigned)(currentIndex + 1), (unsigned)totalItems);
    } else {
        snprintf(header, sizeof(header), "ONLINE %u/%u", (unsigned)(currentIndex + 1), (unsigned)totalItems);
    }
    m_u8g2.drawStr(3, 9, header);

    // Tag badge on right side of header
    const char* badge = "";
    if (isOfflineMode) {
        badge = "FLASH";
    } else if (isOfflineStored) {
        badge = "OFFLINE";
    } else if (isOfflineCompatible) {
        badge = "SPIFFS";
    } else {
        badge = "STREAM";
    }
    m_u8g2.drawStr(128 - m_u8g2.getStrWidth(badge) - 3, 9, badge);
    m_u8g2.setDrawColor(1); // Normal white text

    // Video Title (wrapped if needed)
    m_u8g2.setFont(u8g2_font_6x12_tf);
    if (title && strlen(title) > 0) {
        if (strlen(title) <= 18) {
            m_u8g2.drawStr(4, 30, title);
        } else {
            char line1[32] = {0};
            char line2[32] = {0};
            int splitIdx = 16;
            for (int i = 18; i >= 8; i--) {
                if (title[i] == ' ') { splitIdx = i; break; }
            }
            strncpy(line1, title, splitIdx);
            line1[splitIdx] = '\0';
            const char* rest = title + splitIdx;
            while (*rest == ' ') rest++;
            strncpy(line2, rest, sizeof(line2) - 1);
            line2[sizeof(line2) - 1] = '\0';
            m_u8g2.drawStr(4, 25, line1);
            m_u8g2.drawStr(4, 37, line2);
        }
    } else {
        m_u8g2.drawStr(4, 30, "(Untitled Video)");
    }

    // Sub-info line
    m_u8g2.setFont(u8g2_font_4x6_tf);
    if (isOfflineMode) {
        m_u8g2.drawStr(4, 46, "Stored on Flash (No Wi-Fi)");
    } else if (isOfflineStored) {
        m_u8g2.drawStr(4, 46, "[OFFLINE] Saved on Flash");
    } else if (isOfflineCompatible) {
        m_u8g2.drawStr(4, 46, "SPIFFS OK (Hold 12: Save)");
    } else {
        m_u8g2.drawStr(4, 46, "Stream & Play over Wi-Fi");
    }

    // Bottom Navigation Prompts
    m_u8g2.drawHLine(0, 50, 128);
    m_u8g2.setFont(u8g2_font_5x7_tf);
    m_u8g2.drawStr(2, 60, "[Scroll] Next");
    m_u8g2.drawStr(66, 60, "[Select] Play");

    m_u8g2.sendBuffer();
}

void OLEDDisplay::showOfflineSavedSuccess(const char* title) {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    m_u8g2.drawFrame(0, 0, 128, 64);
    m_u8g2.drawFrame(2, 2, 124, 60);

    m_u8g2.setFont(u8g2_font_6x10_tf);
    m_u8g2.drawStr(6, 18, "DOWNLOAD COMPLETE");

    m_u8g2.setFont(u8g2_font_5x7_tf);
    char buf[32] = {0};
    snprintf(buf, sizeof(buf), "Saved: %.17s", title ? title : "");
    m_u8g2.drawStr(8, 34, buf);
    m_u8g2.drawStr(8, 48, "Saved to Flash!");
    m_u8g2.sendBuffer();
}

void OLEDDisplay::showLoadingCatalogue() {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    m_u8g2.drawFrame(0, 0, 128, 64);
    m_u8g2.drawFrame(2, 2, 124, 60);

    m_u8g2.setFont(u8g2_font_6x10_tf);
    m_u8g2.drawStr(16, 20, "CATALOGUE SYNC");

    m_u8g2.setFont(u8g2_font_5x7_tf);
    m_u8g2.drawStr(18, 36, "Connecting to Web...");
    m_u8g2.drawStr(22, 50, "Fetching Video List");
    m_u8g2.sendBuffer();
}

void OLEDDisplay::showDownloadingScreen(const char* title) {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    m_u8g2.drawFrame(0, 0, 128, 64);
    m_u8g2.drawFrame(2, 2, 124, 60);

    m_u8g2.setFont(u8g2_font_6x10_tf);
    m_u8g2.drawStr(18, 18, "DOWNLOADING...");

    m_u8g2.setFont(u8g2_font_5x7_tf);
    if (title && strlen(title) > 0) {
        char buf[24] = {0};
        strncpy(buf, title, sizeof(buf) - 1);
        int w = m_u8g2.getStrWidth(buf);
        m_u8g2.drawStr((128 - w) / 2, 36, buf);
    }
    m_u8g2.drawStr(14, 52, "Streaming into PSRAM");
    m_u8g2.sendBuffer();
}

void OLEDDisplay::showDownloadProgress(const char* label, size_t currentBytes, size_t totalBytes) {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();

    // Outer double border
    m_u8g2.drawFrame(0, 0, 128, 64);

    // Title / Stage
    m_u8g2.setFont(u8g2_font_6x10_tf);
    m_u8g2.drawStr(14, 14, "DOWNLOADING...");

    // Stream Label (e.g. "Video Stream" or "Audio Stream")
    m_u8g2.setFont(u8g2_font_5x7_tf);
    if (label) {
        int w = m_u8g2.getStrWidth(label);
        m_u8g2.drawStr((128 - w) / 2, 26, label);
    }

    // Progress Bar: 104px wide at x=12, y=30, h=11
    int barX = 12;
    int barY = 30;
    int barW = 104;
    int barH = 11;
    m_u8g2.drawFrame(barX, barY, barW, barH);
    if (totalBytes > 0) {
        int fillW = (int)((uint64_t)currentBytes * (barW - 4) / totalBytes);
        if (fillW > (barW - 4)) fillW = barW - 4;
        if (fillW > 0) {
            m_u8g2.drawBox(barX + 2, barY + 2, fillW, barH - 4);
        }
    }

    // Percentage & byte stats
    char stats[36];
    uint32_t percent = totalBytes > 0 ? (uint32_t)((uint64_t)currentBytes * 100 / totalBytes) : 0;
    snprintf(stats, sizeof(stats), "%u%% (%u / %u KB)",
             percent, (unsigned)(currentBytes / 1024), (unsigned)(totalBytes / 1024));
    m_u8g2.setFont(u8g2_font_4x6_tf);
    int sw = m_u8g2.getStrWidth(stats);
    m_u8g2.drawStr((128 - sw) / 2, 53, stats);

    m_u8g2.sendBuffer();
}

void OLEDDisplay::showPauseScreen(const char* title) {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    m_u8g2.drawBox(24, 18, 80, 28);
    m_u8g2.setDrawColor(0);
    m_u8g2.drawFrame(26, 20, 76, 24);
    m_u8g2.setFont(u8g2_font_7x14B_tf);
    m_u8g2.drawStr(36, 37, "PAUSED");
    m_u8g2.setDrawColor(1);
    m_u8g2.setFont(u8g2_font_4x6_tf);
    m_u8g2.drawStr(4, 58, "[Select] Resume | Hold: Stop");
    m_u8g2.sendBuffer();
}

void OLEDDisplay::showPlayingScreen(const char* title) {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    
    // Draw decorative border
    m_u8g2.drawFrame(0, 0, 128, 64);
    m_u8g2.drawFrame(2, 2, 124, 60);

    m_u8g2.setFont(u8g2_font_5x7_tf);
    m_u8g2.drawStr(38, 12, "NOW PLAYING");

    m_u8g2.setFont(u8g2_font_7x14B_tf);
    if (title && strlen(title) > 0) {
        int width = m_u8g2.getStrWidth(title);
        int x = (128 - width) / 2;
        if (x < 4) x = 4;
        m_u8g2.drawStr(x, 38, title);
    } else {
        m_u8g2.drawStr(24, 38, "MEDIA SLOT");
    }

    m_u8g2.setFont(u8g2_font_5x7_tf);
    m_u8g2.drawStr(25, 54, "STIK + PCM BT");
    m_u8g2.sendBuffer();
}

void OLEDDisplay::showErrorScreen(const char* errorMsg) {
    if (!m_displayConnected) return;
    m_u8g2.clearBuffer();
    m_u8g2.setFont(u8g2_font_7x14B_tf);
    m_u8g2.drawStr(30, 20, "ERROR!");
    m_u8g2.setFont(u8g2_font_6x10_tf);
    if (errorMsg) {
        m_u8g2.drawStr(0, 45, errorMsg);
    }
    m_u8g2.sendBuffer();
}

void OLEDDisplay::renderFrame(const uint8_t* frame1024) {
    if (!m_displayConnected || !frame1024) return;

    // Send 8 pages of 128 bytes directly to SSD1306 (0x3C)
    // Runs in ~13ms total (compared to 50ms in U8g2)
    for (uint8_t page = 0; page < 8; page++) {
        Wire.beginTransmission(0x3C);
        Wire.write(0x00);        // Command mode
        Wire.write(0xB0 | page); // Set page start address (0..7)
        Wire.write(0x00);        // Lower column 0
        Wire.write(0x10);        // Higher column 0
        Wire.endTransmission();

        Wire.beginTransmission(0x3C);
        Wire.write(0x40);        // Data mode
        Wire.write(frame1024 + (page * 128), 128);
        Wire.endTransmission();
    }
}
