#include "media_downloader.h"
#include <memory>
#include "../wifi/wifi_manager.h"
#include "../display/oled_display.h"
#include "../audio/bluetooth/bluetooth.h"
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include "offline_storage.h"
#include <esp_bt.h>
#include <esp_bt_main.h>

static void suspendBluetoothForDownload() {
    Serial.println("[DOWNLOAD] Pausing Bluetooth & releasing controller RAM for HTTPS TLS...");
    bluetoothAudioEnd();
    delay(50);
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_bluedroid_disable();
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
        esp_bluedroid_deinit();
    }
    btStop();
    delay(50);
    Serial.printf("[MEM] Bluetooth released: Free heap: %u, Largest block: %u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

static void restoreBluetoothAfterDownload(bool waitForReconnect = false) {
    Serial.println("[DOWNLOAD] Restoring Bluetooth stack for playback...");
    btStart();
    bluetoothAudioBegin();
    if (waitForReconnect) {
        Serial.println("[DOWNLOAD] Waiting for Bluetooth speaker to reconnect before starting playback...");
        OLEDDisplay::getInstance().showStatusScreen("Bluetooth Audio", "Reconnecting...", "Please wait");
        uint32_t waitStart = millis();
        while (!bluetoothAudioStreaming() && (millis() - waitStart < 15000)) {
            bluetoothAudioUpdate();
            delay(20);
        }
        if (bluetoothAudioStreaming()) {
            Serial.println("[DOWNLOAD] Bluetooth speaker reconnected & streaming!");
        } else {
            Serial.println("[DOWNLOAD] Bluetooth reconnect wait finished; playback will auto-sync on connect.");
        }
    }
}

bool MediaDownloader::downloadToPSRAM(const String& url, uint8_t*& outBuffer, size_t& outSize, size_t maxSize, const char* label) {
    if (!WiFiManager::getInstance().isConnected()) {
        Serial.printf("[DOWNLOAD] %s Error: Wi-Fi not connected.\n", label);
        return false;
    }

    std::unique_ptr<WiFiClient> client;
    std::unique_ptr<WiFiClientSecure> secureClient;
    std::unique_ptr<HTTPClient> http(new HTTPClient());
#define CLEANUP_HTTP() do { if(http) { http->end(); http.reset(); } secureClient.reset(); client.reset(); } while(0)

    http->setTimeout(DOWNLOAD_TIMEOUT_MS);
    bool beginSuccess = false;
    if (url.startsWith("https://")) {
        secureClient.reset(new WiFiClientSecure());
        secureClient->setInsecure();
        beginSuccess = http->begin(*secureClient, url);
    } else {
        client.reset(new WiFiClient());
        beginSuccess = http->begin(*client, url);
    }

    if (!beginSuccess) {
        Serial.printf("[DOWNLOAD] %s Error: Failed to initialize HTTP connection.\n", label);
        CLEANUP_HTTP();
        return false;
    }

    http->setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http->addHeader("X-ESP-CLIENT", "ESP32-CAM");

    const char* headerKeys[] = {"Content-Type"};
    http->collectHeaders(headerKeys, 1);

    int httpCode = http->GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[DOWNLOAD] %s Error: HTTP GET failed for %s, status: %d (%s)\n",
                      label, url.c_str(), httpCode, http->errorToString(httpCode).c_str());
        CLEANUP_HTTP();
        return false;
    }

    int contentLength = http->getSize();
    if (contentLength <= 0) {
        Serial.printf("[DOWNLOAD] %s Error: Invalid or missing Content-Length (%d).\n", label, contentLength);
        CLEANUP_HTTP();
        return false;
    }

    size_t requiredSize = (size_t)contentLength;
    Serial.printf("[DOWNLOAD] URL: %s\n", url.c_str());
    Serial.printf("[DOWNLOAD] HTTP status: %d\n", httpCode);
    Serial.printf("[DOWNLOAD] Content-Type: %s\n", http->hasHeader("Content-Type") ? http->header("Content-Type").c_str() : "unknown");
    Serial.printf("[DOWNLOAD] Content-Length: %d\n", contentLength);
    Serial.printf("[DOWNLOAD] Free heap before: %u\n", ESP.getFreeHeap());
    Serial.printf("[DOWNLOAD] Largest heap block before: %u\n", ESP.getMaxAllocHeap());
    Serial.printf("[DOWNLOAD] Free PSRAM before: %u\n", ESP.getFreePsram());

    if (requiredSize > maxSize) {
        Serial.printf("[DOWNLOAD] %s REJECTED: Size %u exceeds max limit %u bytes.\n",
                      label, (unsigned)requiredSize, (unsigned)maxSize);
        CLEANUP_HTTP();
        return false;
    }

    if (!psramFound()) {
        Serial.println("[PSRAM] REJECTED: PSRAM not found!");
        CLEANUP_HTTP();
        return false;
    }

    size_t freePsram = ESP.getFreePsram();
    Serial.printf("[PSRAM] Free before %s allocation: %u bytes\n", label, (unsigned)freePsram);

    if (freePsram < requiredSize + PSRAM_SAFETY_MARGIN_BYTES) {
        Serial.printf("[PSRAM] REJECTED: Insufficient PSRAM for %s (Required: %u bytes, Free: %u bytes)\n",
                      label, (unsigned)(requiredSize + PSRAM_SAFETY_MARGIN_BYTES), (unsigned)freePsram);
        CLEANUP_HTTP();
        return false;
    }

    outBuffer = (uint8_t*)ps_malloc(requiredSize);
    if (!outBuffer) {
        Serial.printf("[PSRAM] Error: Failed to allocate %u bytes in PSRAM for %s.\n", (unsigned)requiredSize, label);
        CLEANUP_HTTP();
        return false;
    }

    Serial.printf("[DOWNLOAD] Downloading %s into PSRAM...\n", label);
    OLEDDisplay::getInstance().showDownloadProgress(label, 0, requiredSize);

    WiFiClient* stream = http->getStreamPtr();
    size_t bytesReadTotal = 0;
    uint32_t startMs = millis();
    uint32_t lastOledMs = 0;
    const size_t chunkSize = 4096;

    while (http->connected() && (bytesReadTotal < requiredSize)) {
        size_t availableBytes = stream->available();
        if (availableBytes > 0) {
            size_t toRead = std::min(availableBytes, std::min(chunkSize, requiredSize - bytesReadTotal));
            int readCount = stream->read(outBuffer + bytesReadTotal, toRead);
            if (readCount > 0) {
                bytesReadTotal += readCount;
                startMs = millis();

                // Smoothly update OLED progress bar every 120ms
                if ((millis() - lastOledMs >= 120) || (bytesReadTotal >= requiredSize)) {
                    lastOledMs = millis();
                    OLEDDisplay::getInstance().showDownloadProgress(label, bytesReadTotal, requiredSize);
                }
            } else if (readCount < 0) {
                Serial.printf("[DOWNLOAD] %s Error: Socket read error.\n", label);
                break;
            }
        } else {
            delay(1);
            if (millis() - startMs > DOWNLOAD_TIMEOUT_MS) {
                Serial.printf("[DOWNLOAD] %s Error: Stream read timeout.\n", label);
                break;
            }
        }
    }

    CLEANUP_HTTP();
#undef CLEANUP_HTTP

    if (bytesReadTotal != requiredSize) {
        Serial.printf("[DOWNLOAD] %s Error: Short read! Expected %u bytes, received %u bytes.\n",
                      label, (unsigned)requiredSize, (unsigned)bytesReadTotal);
        free(outBuffer);
        outBuffer = nullptr;
        return false;
    }

    outSize = requiredSize;
    Serial.printf("[DOWNLOAD] Received: %u / %u\n", (unsigned)bytesReadTotal, (unsigned)requiredSize);
    Serial.printf("[DOWNLOAD] Free PSRAM after: %u\n", ESP.getFreePsram());
    Serial.printf("[DOWNLOAD] COMPLETE\n");
    return true;
}

bool MediaDownloader::validateSTIK(const uint8_t* buffer, size_t size) {
    if (buffer == nullptr || size < 16) {
        Serial.println("[VALIDATE] STIK FAILED: File size less than 16 bytes.");
        return false;
    }

    // Magic: "STIK"
    if (buffer[0] != 'S' || buffer[1] != 'T' || buffer[2] != 'I' || buffer[3] != 'K') {
        Serial.println("[VALIDATE] STIK FAILED: Invalid magic signature.");
        return false;
    }

    uint16_t width = (uint16_t)buffer[4] | ((uint16_t)buffer[5] << 8);
    uint16_t height = (uint16_t)buffer[6] | ((uint16_t)buffer[7] << 8);
    uint8_t fps = buffer[8];
    uint32_t frameCount = (uint32_t)buffer[9] | ((uint32_t)buffer[10] << 8) |
                          ((uint32_t)buffer[11] << 16) | ((uint32_t)buffer[12] << 24);

    Serial.printf("[VALIDATE] STIK Header: %ux%u @ %u FPS, %u frames, Size: %u bytes\n",
                  width, height, fps, frameCount, (unsigned)size);

    if (width != 128 || height != 64) {
        Serial.printf("[VALIDATE] STIK FAILED: Unsupported resolution %ux%u (expected 128x64).\n", width, height);
        return false;
    }

    if (fps == 0) {
        Serial.println("[VALIDATE] STIK FAILED: FPS is 0.");
        return false;
    }

    if (frameCount == 0) {
        Serial.println("[VALIDATE] STIK FAILED: Frame count is 0.");
        return false;
    }

    Serial.println("[VALIDATE] STIK OK");
    return true;
}

bool MediaDownloader::validatePCM(const uint8_t* buffer, size_t size) {
    if (buffer == nullptr || size == 0) {
        Serial.println("[VALIDATE] PCM FAILED: Audio buffer is empty.");
        return false;
    }

    Serial.printf("[VALIDATE] PCM Audio OK: %u bytes\n", (unsigned)size);
    return true;
}

bool MediaDownloader::validateSTIKFile(const String& filePath) {
    if (!LittleFS.exists(filePath)) return false;
    File f = LittleFS.open(filePath, FILE_READ);
    if (!f || f.size() < 16) {
        if (f) f.close();
        return false;
    }

    uint8_t header[16];
    size_t readCount = f.read(header, 16);
    f.close();

    if (readCount < 16) return false;
    return validateSTIK(header, readCount);
}

bool MediaDownloader::downloadToFile(const String& url, const String& filePath, size_t maxSize, const char* label, size_t& outSize, bool allowPartial) {
    outSize = 0;
    if (!WiFiManager::getInstance().isConnected()) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Wi-Fi not connected.\n", label);
        return false;
    }

    if (!OfflineStorage::getInstance().isReady()) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Filesystem is not mounted! (Run 'fs format' in serial monitor)\n", label);
        return false;
    }

    std::unique_ptr<WiFiClient> client;
    std::unique_ptr<WiFiClientSecure> secureClient;
    std::unique_ptr<HTTPClient> http(new HTTPClient());
#define CLEANUP_FLASH_HTTP() do { if(http) { http->end(); http.reset(); } secureClient.reset(); client.reset(); } while(0)

    http->setTimeout(DOWNLOAD_TIMEOUT_MS);
    bool beginSuccess = false;
    if (url.startsWith("https://")) {
        secureClient.reset(new WiFiClientSecure());
        secureClient->setInsecure();
        beginSuccess = http->begin(*secureClient, url);
    } else {
        client.reset(new WiFiClient());
        beginSuccess = http->begin(*client, url);
    }

    if (!beginSuccess) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Failed to initialize HTTP connection.\n", label);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    http->setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http->addHeader("X-ESP-CLIENT", "ESP32-CAM");

    const char* headerKeys[] = {"Content-Type"};
    http->collectHeaders(headerKeys, 1);

    int httpCode = http->GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: HTTP GET failed for %s, status: %d (%s)\n",
                      label, url.c_str(), httpCode, http->errorToString(httpCode).c_str());
        CLEANUP_FLASH_HTTP();
        return false;
    }

    int contentLength = http->getSize();
    if (contentLength <= 0) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Invalid or missing Content-Length (%d).\n", label, contentLength);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    size_t requiredSize = (size_t)contentLength;
    Serial.printf("[DOWNLOAD-FLASH] %s URL: %s\n", label, url.c_str());
    Serial.printf("[DOWNLOAD-FLASH] %s size: %u bytes\n", label, (unsigned)requiredSize);

    if (requiredSize > maxSize) {
        Serial.printf("[DOWNLOAD-FLASH] %s REJECTED: Size %u exceeds max limit %u bytes.\n",
                      label, (unsigned)requiredSize, (unsigned)maxSize);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    size_t totalFlash = LittleFS.totalBytes();
    size_t usedFlash  = LittleFS.usedBytes();
    size_t freeFlash  = (totalFlash > usedFlash) ? (totalFlash - usedFlash) : 0;

    // If downloading a new stream video and temporary stream cache is occupying needed flash space,
    // reclaim temporary stream cache ONLY if the new file actually fits in the partition.
    if (filePath == "/stream_video.bin.tmp" && freeFlash < requiredSize + 4096) {
        if (LittleFS.exists("/stream_audio.bin")) {
            LittleFS.remove("/stream_audio.bin");
            usedFlash = LittleFS.usedBytes();
            freeFlash = (totalFlash > usedFlash) ? (totalFlash - usedFlash) : 0;
        }
        if (freeFlash < requiredSize + 4096 && totalFlash >= requiredSize + 20480 && LittleFS.exists("/stream_video.bin")) {
            LittleFS.remove("/stream_video.bin");
            usedFlash = LittleFS.usedBytes();
            freeFlash = (totalFlash > usedFlash) ? (totalFlash - usedFlash) : 0;
        }
    }

    if (allowPartial && freeFlash > 16384 && requiredSize + 8192 > freeFlash) {
        size_t cappedSize = freeFlash - 8192;
        Serial.printf("[DOWNLOAD-FLASH] %s: Capping stream size from %u to %u bytes to fit remaining SPIFFS space.\n",
                      label, (unsigned)requiredSize, (unsigned)cappedSize);
        requiredSize = cappedSize;
    }

    Serial.printf("[DOWNLOAD-FLASH] Heap: %u (largest %u), SPIFFS: Total %u, Used %u, Free %u bytes\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                  (unsigned)totalFlash, (unsigned)usedFlash, (unsigned)freeFlash);

    if (freeFlash < requiredSize + 4096) {
        Serial.printf("[DOWNLOAD-FLASH] REJECTED: Insufficient flash space (Required: %u, Free: %u bytes)\n",
                      (unsigned)(requiredSize + 4096), (unsigned)freeFlash);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    if (LittleFS.exists(filePath)) {
        LittleFS.remove(filePath);
    }

    File f = LittleFS.open(filePath, FILE_WRITE);
    if (!f) {
        Serial.printf("[DOWNLOAD-FLASH] Failed to create file '%s' on LittleFS.\n", filePath.c_str());
        CLEANUP_FLASH_HTTP();
        return false;
    }

    const size_t chunkSize = 2048;
    std::unique_ptr<uint8_t[]> chunkBuf(new (std::nothrow) uint8_t[chunkSize]);
    if (!chunkBuf) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Failed to allocate 2KB chunk buffer.\n", label);
        f.close();
        LittleFS.remove(filePath);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    Serial.printf("[DOWNLOAD-FLASH] Streaming %s into LittleFS '%s'...\n", label, filePath.c_str());
    OLEDDisplay::getInstance().showDownloadProgress(label, 0, requiredSize);

    WiFiClient* stream = http->getStreamPtr();
    size_t bytesReadTotal = 0;
    uint32_t startMs = millis();
    uint32_t lastOledMs = 0;
    bool writeFailure = false;

    while (http->connected() && (bytesReadTotal < requiredSize)) {
        size_t availableBytes = stream->available();
        if (availableBytes > 0) {
            size_t toRead = std::min(availableBytes, std::min(chunkSize, requiredSize - bytesReadTotal));
            int readCount = stream->read(chunkBuf.get(), toRead);
            if (readCount > 0) {
                size_t written = f.write(chunkBuf.get(), readCount);
                if (written != (size_t)readCount) {
                    Serial.printf("[DOWNLOAD-FLASH] %s Error: Filesystem write error (wrote %u / %d bytes)!\n",
                                  label, (unsigned)written, readCount);
                    writeFailure = true;
                    break;
                }
                bytesReadTotal += readCount;
                startMs = millis();

                if ((millis() - lastOledMs >= 120) || (bytesReadTotal >= requiredSize)) {
                    lastOledMs = millis();
                    OLEDDisplay::getInstance().showDownloadProgress(label, bytesReadTotal, requiredSize);
                }
            } else if (readCount < 0) {
                Serial.printf("[DOWNLOAD-FLASH] %s Error: Socket read error.\n", label);
                break;
            }
        } else {
            delay(1);
            if (millis() - startMs > DOWNLOAD_TIMEOUT_MS) {
                Serial.printf("[DOWNLOAD-FLASH] %s Error: Stream read timeout.\n", label);
                break;
            }
        }
    }

    f.flush();
    f.close();
    CLEANUP_FLASH_HTTP();
#undef CLEANUP_FLASH_HTTP

    if (writeFailure || bytesReadTotal != requiredSize) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: %s! Expected %u, wrote %u bytes.\n",
                      label, writeFailure ? "Write error" : "Short read",
                      (unsigned)requiredSize, (unsigned)bytesReadTotal);
        LittleFS.remove(filePath);
        return false;
    }

    outSize = requiredSize;
    Serial.printf("[DOWNLOAD-FLASH] %s complete (%u bytes written to %s)\n", label, (unsigned)outSize, filePath.c_str());
    return true;
}

bool MediaDownloader::downloadMedia(const MediaItem& item, DownloadedMedia& outResult) {
    if (!item.isValid()) {
        Serial.println("[DOWNLOAD] Error: Invalid MediaItem details.");
        return false;
    }

    bool wasBtActive = (bluetoothAudioGetState() != BluetoothState::OFF);
    bool wasBtConnected = bluetoothAudioConnected();
    if (wasBtActive) {
        suspendBluetoothForDownload();
    }

    struct BtRestorer {
        bool active;
        bool connected;
        ~BtRestorer() {
            if (active) {
                restoreBluetoothAfterDownload(connected);
            }
        }
    } btRestorer{wasBtActive, wasBtConnected};

    Serial.println();
    Serial.println("========================================");
    Serial.printf("[CONTENT] Selected Item:\n  ID:   %s\n  Name: %s\n", item.id.c_str(), item.name.c_str());
    Serial.println("========================================");

    Serial.printf("[MEM] Free heap before: %u, largest block: %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    DownloadedMedia tempMedia;
    tempMedia.item = item;

    const String finalVideoPath = "/stream_video.bin";
    const String tmpVideoPath = "/stream_video.bin.tmp";

    // 1. Download video directly into SPIFFS filesystem file using small bounded buffer
    size_t vSize = 0;
    if (!downloadToFile(item.videoUrl, tmpVideoPath, MAX_VIDEO_SIZE, "Video", vSize)) {
        Serial.println("[DOWNLOAD] Video download to SPIFFS failed.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        return false;
    }

    // 2. Validate STIK header of the temporary file before touching final file
    if (!validateSTIKFile(tmpVideoPath)) {
        Serial.println("[CONTENT] STIK validation FAILED on downloaded file.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        return false;
    }

    // 3. Atomically replace final video file
    if (LittleFS.exists(finalVideoPath)) {
        LittleFS.remove(finalVideoPath);
    }
    if (!LittleFS.rename(tmpVideoPath, finalVideoPath)) {
        Serial.println("[DOWNLOAD] Error: Failed to rename temp video file to final path.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        return false;
    }

    tempMedia.videoFilePath = finalVideoPath;
    tempMedia.videoSize = vSize;
    tempMedia.videoBuffer = nullptr;

    // 4. Audio download:
    // If PSRAM is available (ESP32-CAM target), download audio into PSRAM.
    // If PSRAM is absent (ESP32-WROOM target), stream audio into SPIFFS for ring-buffered BT playback.
    if (psramFound()) {
        if (!downloadToPSRAM(item.audioUrl, tempMedia.audioBuffer, tempMedia.audioSize, MAX_AUDIO_SIZE, "Audio")) {
            Serial.println("[DOWNLOAD] Audio download failed (continuing video without audio).");
            tempMedia.audioBuffer = nullptr;
            tempMedia.audioSize = 0;
        } else {
            validatePCM(tempMedia.audioBuffer, tempMedia.audioSize);
        }
    } else if (item.audioUrl.length() > 0) {
        const String finalAudioPath = "/stream_audio.bin";
        const String tmpAudioPath = "/stream_audio.bin.tmp";
        if (LittleFS.exists(finalAudioPath)) LittleFS.remove(finalAudioPath);
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);

        size_t aSize = 0;
        if (downloadToFile(item.audioUrl, tmpAudioPath, MAX_AUDIO_SIZE, "Audio", aSize, true) && aSize > 0) {
            if (LittleFS.rename(tmpAudioPath, finalAudioPath)) {
                tempMedia.audioFilePath = finalAudioPath;
                tempMedia.audioSize = aSize;
                Serial.printf("[DOWNLOAD] WROOM-32: Audio saved to SPIFFS (%u bytes) for Bluetooth playback!\n", (unsigned)aSize);
            } else {
                if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
            }
        } else {
            if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
            Serial.println("[DOWNLOAD] WROOM-32: Audio download skipped or no space remaining (continuing video without audio).");
        }
        tempMedia.audioBuffer = nullptr;
    } else {
        tempMedia.audioBuffer = nullptr;
        tempMedia.audioSize = 0;
    }

    // Transfer ownership on clean success
    outResult = tempMedia;
    Serial.printf("[DOWNLOAD] Video downloaded and validated successfully in SPIFFS! (%u bytes)\n", (unsigned)vSize);
    return true;
}

bool MediaDownloader::downloadToFlash(const MediaItem& item, const String& videoFilePath, const String& audioFilePath,
                                     size_t& outVideoSize, size_t& outAudioSize) {
    if (!item.isValid()) {
        Serial.println("[DOWNLOAD-FLASH] Error: Invalid MediaItem details.");
        return false;
    }

    bool wasBtActive = (bluetoothAudioGetState() != BluetoothState::OFF);
    bool wasBtConnected = bluetoothAudioConnected();
    if (wasBtActive) {
        suspendBluetoothForDownload();
    }

    struct BtRestorer {
        bool active;
        bool connected;
        ~BtRestorer() {
            if (active) {
                restoreBluetoothAfterDownload(connected);
            }
        }
    } btRestorer{wasBtActive, wasBtConnected};

    Serial.println();
    Serial.println("========================================");
    Serial.printf("[DOWNLOAD-FLASH] Storing item to Flash:\n  ID:   %s\n  Name: %s\n", item.id.c_str(), item.name.c_str());
    Serial.println("========================================");

    String tmpVideoPath = videoFilePath + ".tmp";
    String tmpAudioPath = audioFilePath + ".tmp";

    // Clean up any stale temp or stream cache files first so offline storage has full partition space
    if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
    if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
    if (LittleFS.exists("/stream_video.bin.tmp")) LittleFS.remove("/stream_video.bin.tmp");
    if (LittleFS.exists("/stream_audio.bin.tmp")) LittleFS.remove("/stream_audio.bin.tmp");
    if (LittleFS.exists("/stream_audio.bin")) LittleFS.remove("/stream_audio.bin");
    if (LittleFS.exists("/stream_video.bin")) LittleFS.remove("/stream_video.bin");

    // 1. Download video directly to temporary flash file
    size_t vSize = 0;
    if (!downloadToFile(item.videoUrl, tmpVideoPath, OFFLINE_STORAGE_LIMIT_BYTES, "Flash Video", vSize)) {
        Serial.println("[DOWNLOAD-FLASH] Video file download to flash failed.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        return false;
    }

    // 2. Validate STIK header of the temp file on flash
    if (!validateSTIKFile(tmpVideoPath)) {
        Serial.println("[DOWNLOAD-FLASH] Video STIK header validation failed!");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        return false;
    }

    // 3. Download audio directly to temporary flash file
    size_t aSize = 0;
    if (!downloadToFile(item.audioUrl, tmpAudioPath, OFFLINE_STORAGE_LIMIT_BYTES, "Flash Audio", aSize)) {
        Serial.println("[DOWNLOAD-FLASH] Audio file download to flash failed.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
        return false;
    }

    // 4. Validate audio size
    if (aSize == 0) {
        Serial.println("[DOWNLOAD-FLASH] Audio file is empty!");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
        return false;
    }

    // 5. Both files completely downloaded and validated! Clean old offline files and atomically replace final files.
    OfflineStorage::getInstance().clearOfflineStorage();

    // Ensure target directory exists for item
    String itemDir = String(OFFLINE_DIR) + "/" + item.id;
    if (!LittleFS.exists(OFFLINE_DIR)) LittleFS.mkdir(OFFLINE_DIR);
    if (!LittleFS.exists(itemDir)) LittleFS.mkdir(itemDir);

    if (LittleFS.exists(videoFilePath)) LittleFS.remove(videoFilePath);
    if (!LittleFS.rename(tmpVideoPath, videoFilePath)) {
        Serial.println("[DOWNLOAD-FLASH] Error: Failed to rename temp video file.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
        return false;
    }

    if (LittleFS.exists(audioFilePath)) LittleFS.remove(audioFilePath);
    if (!LittleFS.rename(tmpAudioPath, audioFilePath)) {
        Serial.println("[DOWNLOAD-FLASH] Error: Failed to rename temp audio file.");
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
        return false;
    }

    // 6. Physically verify saved files exist and sizes match on disk
    if (!LittleFS.exists(videoFilePath) || !LittleFS.exists(audioFilePath)) {
        Serial.println("[DOWNLOAD-FLASH] Error: Saved files missing after rename!");
        return false;
    }

    File vf = LittleFS.open(videoFilePath, FILE_READ);
    File af = LittleFS.open(audioFilePath, FILE_READ);
    if (!vf || !af) {
        Serial.println("[DOWNLOAD-FLASH] Error: Cannot open saved files for verification!");
        if (vf) vf.close();
        if (af) af.close();
        return false;
    }
    size_t actualV = vf.size();
    size_t actualA = af.size();
    vf.close();
    af.close();

    if (actualV != vSize || actualA != aSize) {
        Serial.printf("[DOWNLOAD-FLASH] Verification failed! Expected v=%u a=%u, got v=%u a=%u\n",
                      (unsigned)vSize, (unsigned)aSize, (unsigned)actualV, (unsigned)actualA);
        return false;
    }

    outVideoSize = vSize;
    outAudioSize = aSize;
    Serial.printf("[DOWNLOAD-FLASH] Item '%s' saved to LittleFS! Total: %u bytes\n",
                  item.name.c_str(), (unsigned)(vSize + aSize));
    return true;
}
