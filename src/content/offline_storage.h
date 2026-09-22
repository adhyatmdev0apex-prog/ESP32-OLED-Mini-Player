#ifndef OFFLINE_STORAGE_H
#define OFFLINE_STORAGE_H

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include "media_item.h"
#include "media_downloader.h"
#include "../config/app_config.h"

class OfflineStorage {
public:
    static OfflineStorage& getInstance();

    bool begin();
    bool isReady() const { return m_mounted; }

    size_t getTotalBytes() const;
    size_t getUsedBytes() const;
    size_t getFreeBytes() const;

    void printDiagnostics();

    bool isItemStored(const String& id) const;
    size_t getStoredCount() const { return m_offlineCatalogue.items.size(); }
    const CataloguePage& getOfflineCatalogue() const { return m_offlineCatalogue; }

    String getVideoFilePath(const String& id) const { return String(OFFLINE_DIR) + "/" + id + "/video.bin"; }
    String getAudioFilePath(const String& id) const { return String(OFFLINE_DIR) + "/" + id + "/audio.bin"; }
    String getLegacyVideoFilePath(const String& id) const { return "/off_" + id + "_v.bin"; }
    String getLegacyAudioFilePath(const String& id) const { return "/off_" + id + "_a.bin"; }

    bool registerDownloadedItem(const MediaItem& item, size_t videoSize, size_t audioSize);
    bool removeStoredItem(const String& id);
    bool clearOfflineStorage();
    bool verifyMediaFiles(const String& id, size_t expectedVideoSize, size_t expectedAudioSize);

    bool loadItemToPSRAM(const MediaItem& item, DownloadedMedia& outResult);

    // Rebuild index from disk files if catalogue JSON is missing or corrupt
    bool rebuildCatalogueFromDisk();

    // Manual format ONLY via explicit serial command
    bool manualFormat();

private:
    OfflineStorage();
    ~OfflineStorage() = default;

    OfflineStorage(const OfflineStorage&) = delete;
    OfflineStorage& operator=(const OfflineStorage&) = delete;

    bool loadCatalogueFromFlash();
    bool saveCatalogueToFlash();
    void recursiveListDir(const char* dirPath, int depth);

    bool m_mounted;
    CataloguePage m_offlineCatalogue;
};

#endif // OFFLINE_STORAGE_H
