#include "offline_storage.h"

OfflineStorage& OfflineStorage::getInstance() {
    static OfflineStorage instance;
    return instance;
}

OfflineStorage::OfflineStorage()
    : m_mounted(false) {
}

bool OfflineStorage::begin() {
    Serial.println("[FS] Mounting...");

    // CRITICAL: formatOnFail MUST BE FALSE!
    // Never automatically format the offline filesystem on boot.
    // A mount failure must produce an error, NOT erase user's offline media.
    if (!LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
        Serial.println("[FS] Mount FAIL");
        Serial.println("[FS] Warning: LittleFS partition could not be mounted.");
        m_mounted = false;
        return false;
    }

    m_mounted = true;
    size_t total = getTotalBytes();
    size_t used = getUsedBytes();
    size_t freeBytes = getFreeBytes();

    Serial.println("[FS] Mount SUCCESS");
    Serial.printf("[FS] Total bytes: %u\n", (unsigned)total);
    Serial.printf("[FS] Used bytes: %u\n", (unsigned)used);
    Serial.printf("[FS] Free bytes: %u\n", (unsigned)freeBytes);

    // Ensure /offline base directory exists
    if (!LittleFS.exists(OFFLINE_DIR)) {
        LittleFS.mkdir(OFFLINE_DIR);
    }

    // Print full filesystem diagnostics / file listing
    printDiagnostics();

    // Load offline catalogue
    if (!loadCatalogueFromFlash() || m_offlineCatalogue.total == 0) {
        Serial.println("[FS] Catalogue empty or missing. Scanning filesystem to rebuild...");
        rebuildCatalogueFromDisk();
    }

    return true;
}

size_t OfflineStorage::getTotalBytes() const {
    return m_mounted ? LittleFS.totalBytes() : 0;
}

size_t OfflineStorage::getUsedBytes() const {
    return m_mounted ? LittleFS.usedBytes() : 0;
}

size_t OfflineStorage::getFreeBytes() const {
    if (!m_mounted) return 0;
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    return (total > used) ? (total - used) : 0;
}

void OfflineStorage::recursiveListDir(const char* dirPath, int depth) {
    if (!m_mounted || depth > 3) return;

    File dir = LittleFS.open(dirPath);
    if (!dir) return;

    if (!dir.isDirectory()) {
        dir.close();
        return;
    }

    File file = dir.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            Serial.printf("[FS] %s/\n", file.path());
            recursiveListDir(file.path(), depth + 1);
        } else {
            Serial.printf("[FS] %-34s %8u bytes\n", file.path(), (unsigned)file.size());
        }
        file = dir.openNextFile();
    }
    dir.close();
}

void OfflineStorage::printDiagnostics() {
    if (!m_mounted) {
        Serial.println("[FS] Cannot list: Filesystem not mounted.");
        return;
    }

    Serial.println("----------------------------------------");
    Serial.println("[FS] Persistent Filesystem Contents:");
    recursiveListDir("/", 0);
    Serial.println("----------------------------------------");
}

bool OfflineStorage::isItemStored(const String& id) const {
    if (!m_mounted || id.length() == 0) return false;

    // Check directory structure first: /offline/<id>/video.bin & audio.bin
    String vPath = getVideoFilePath(id);
    String aPath = getAudioFilePath(id);
    if (LittleFS.exists(vPath) && LittleFS.exists(aPath)) return true;

    // Check legacy flat path: /off_<id>_v.bin & /off_<id>_a.bin
    String legV = getLegacyVideoFilePath(id);
    String legA = getLegacyAudioFilePath(id);
    if (LittleFS.exists(legV) && LittleFS.exists(legA)) return true;

    return false;
}

bool OfflineStorage::loadCatalogueFromFlash() {
    m_offlineCatalogue.items.clear();
    m_offlineCatalogue.page = 0;
    m_offlineCatalogue.total = 0;

    if (!m_mounted) return false;

    String catPath = OFFLINE_CATALOGUE_PATH;
    if (!LittleFS.exists(catPath)) {
        if (LittleFS.exists(LEGACY_OFFLINE_CATALOGUE_PATH)) {
            catPath = LEGACY_OFFLINE_CATALOGUE_PATH;
        } else {
            Serial.println("[STORAGE] No offline catalogue file found on flash.");
            return false;
        }
    }

    File f = LittleFS.open(catPath, FILE_READ);
    if (!f) {
        Serial.printf("[STORAGE] Failed to open %s for reading.\n", catPath.c_str());
        return false;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[STORAGE] Failed to parse offline catalogue JSON: %s\n", err.c_str());
        return false;
    }

    JsonArray itemsArr = doc["items"].as<JsonArray>();
    for (JsonObject obj : itemsArr) {
        MediaItem it;
        it.id = obj["id"] | "";
        it.name = obj["name"] | "";
        it.isSpiffsCompatible = true;
        it.isOfflineStored = true;
        it.totalSize = obj["total_size"] | 0;

        // Resolve paths on flash
        String vPath = getVideoFilePath(it.id);
        String aPath = getAudioFilePath(it.id);
        if (!LittleFS.exists(vPath) || !LittleFS.exists(aPath)) {
            String legV = getLegacyVideoFilePath(it.id);
            String legA = getLegacyAudioFilePath(it.id);
            if (LittleFS.exists(legV) && LittleFS.exists(legA)) {
                vPath = legV;
                aPath = legA;
            } else {
                vPath = obj["video_path"] | "";
                aPath = obj["audio_path"] | "";
            }
        }

        it.videoUrl = vPath;
        it.audioUrl = aPath;

        if (it.isValid() && LittleFS.exists(vPath) && LittleFS.exists(aPath)) {
            m_offlineCatalogue.items.push_back(it);
        }
    }

    m_offlineCatalogue.total = m_offlineCatalogue.items.size();
    Serial.printf("[STORAGE] Loaded %u offline media items from flash catalogue.\n", (unsigned)m_offlineCatalogue.total);
    return m_offlineCatalogue.total > 0;
}

bool OfflineStorage::saveCatalogueToFlash() {
    if (!m_mounted) return false;

    if (!LittleFS.exists(OFFLINE_DIR)) {
        LittleFS.mkdir(OFFLINE_DIR);
    }

    File f = LittleFS.open(OFFLINE_CATALOGUE_PATH, FILE_WRITE);
    if (!f) {
        Serial.printf("[STORAGE] Failed to open %s for writing.\n", OFFLINE_CATALOGUE_PATH);
        return false;
    }

    DynamicJsonDocument doc(4096);
    doc["total"] = m_offlineCatalogue.items.size();
    JsonArray itemsArr = doc.createNestedArray("items");

    for (const auto& it : m_offlineCatalogue.items) {
        JsonObject obj = itemsArr.createNestedObject();
        obj["id"] = it.id;
        obj["name"] = it.name;
        obj["video_path"] = it.videoUrl;
        obj["audio_path"] = it.audioUrl;
        obj["total_size"] = it.totalSize;
    }

    size_t bytesWritten = serializeJson(doc, f);
    f.flush();
    f.close();

    if (bytesWritten == 0 || !LittleFS.exists(OFFLINE_CATALOGUE_PATH)) {
        Serial.println("[STORAGE] Error: Failed to flush offline catalogue to flash!");
        return false;
    }

    Serial.printf("[STORAGE] Saved offline catalogue to flash (%u items, %u bytes).\n",
                  (unsigned)m_offlineCatalogue.items.size(), (unsigned)bytesWritten);
    return true;
}

bool OfflineStorage::registerDownloadedItem(const MediaItem& item, size_t videoSize, size_t audioSize) {
    if (!m_mounted) return false;

    // Remove existing if replacing
    removeStoredItem(item.id);

    MediaItem stored = item;
    stored.videoUrl = getVideoFilePath(item.id);
    stored.audioUrl = getAudioFilePath(item.id);
    stored.isSpiffsCompatible = true;
    stored.isOfflineStored = true;
    stored.totalSize = videoSize + audioSize;

    m_offlineCatalogue.items.push_back(stored);
    m_offlineCatalogue.total = m_offlineCatalogue.items.size();

    return saveCatalogueToFlash();
}

bool OfflineStorage::removeStoredItem(const String& id) {
    if (!m_mounted) return false;

    String vPath = getVideoFilePath(id);
    String aPath = getAudioFilePath(id);
    String legV = getLegacyVideoFilePath(id);
    String legA = getLegacyAudioFilePath(id);

    if (LittleFS.exists(vPath)) LittleFS.remove(vPath);
    if (LittleFS.exists(aPath)) LittleFS.remove(aPath);
    if (LittleFS.exists(legV)) LittleFS.remove(legV);
    if (LittleFS.exists(legA)) LittleFS.remove(legA);

    String dirPath = String(OFFLINE_DIR) + "/" + id;
    if (LittleFS.exists(dirPath)) LittleFS.rmdir(dirPath);

    for (auto it = m_offlineCatalogue.items.begin(); it != m_offlineCatalogue.items.end(); ++it) {
        if (it->id == id) {
            m_offlineCatalogue.items.erase(it);
            m_offlineCatalogue.total = m_offlineCatalogue.items.size();
            saveCatalogueToFlash();
            return true;
        }
    }
    return false;
}

bool OfflineStorage::clearOfflineStorage() {
    if (!m_mounted) return false;

    Serial.println("[STORAGE] Cleaning old offline video/audio files from LittleFS...");

    // 1. Remove all items from catalogue
    for (const auto& it : m_offlineCatalogue.items) {
        String vPath = getVideoFilePath(it.id);
        String aPath = getAudioFilePath(it.id);
        String legV = getLegacyVideoFilePath(it.id);
        String legA = getLegacyAudioFilePath(it.id);
        if (LittleFS.exists(vPath)) LittleFS.remove(vPath);
        if (LittleFS.exists(aPath)) LittleFS.remove(aPath);
        if (LittleFS.exists(legV)) LittleFS.remove(legV);
        if (LittleFS.exists(legA)) LittleFS.remove(legA);
        String subDir = String(OFFLINE_DIR) + "/" + it.id;
        if (LittleFS.exists(subDir)) LittleFS.rmdir(subDir);
    }

    // 2. Scan /offline directory and delete any leftover files
    File offlineDir = LittleFS.open(OFFLINE_DIR);
    if (offlineDir && offlineDir.isDirectory()) {
        File sub = offlineDir.openNextFile();
        while (sub) {
            String p = sub.path();
            if (sub.isDirectory()) {
                File inner = sub.openNextFile();
                while (inner) {
                    LittleFS.remove(inner.path());
                    inner = sub.openNextFile();
                }
                LittleFS.rmdir(p);
            } else {
                LittleFS.remove(p);
            }
            sub = offlineDir.openNextFile();
        }
        offlineDir.close();
    }

    // 3. Remove catalogue files and legacy files
    if (LittleFS.exists(OFFLINE_CATALOGUE_PATH)) LittleFS.remove(OFFLINE_CATALOGUE_PATH);
    if (LittleFS.exists(LEGACY_OFFLINE_CATALOGUE_PATH)) LittleFS.remove(LEGACY_OFFLINE_CATALOGUE_PATH);

    // 4. Also scan root for any orphaned .tmp or /off_ files
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            String name = f.name();
            String path = f.path();
            if (name.startsWith("off_") || name.endsWith(".tmp")) {
                LittleFS.remove(path);
            }
            f = root.openNextFile();
        }
        root.close();
    }

    m_offlineCatalogue.items.clear();
    m_offlineCatalogue.total = 0;

    // Re-create /offline clean directory
    if (!LittleFS.exists(OFFLINE_DIR)) {
        LittleFS.mkdir(OFFLINE_DIR);
    }

    Serial.printf("[STORAGE] Flash cleanup complete. Free bytes: %u\n", (unsigned)getFreeBytes());
    return true;
}

bool OfflineStorage::verifyMediaFiles(const String& id, size_t expectedVideoSize, size_t expectedAudioSize) {
    if (!m_mounted) return false;

    String vPath = getVideoFilePath(id);
    String aPath = getAudioFilePath(id);

    if (!LittleFS.exists(vPath) || !LittleFS.exists(aPath)) {
        String legV = getLegacyVideoFilePath(id);
        String legA = getLegacyAudioFilePath(id);
        if (LittleFS.exists(legV) && LittleFS.exists(legA)) {
            vPath = legV;
            aPath = legA;
        } else {
            Serial.printf("[VERIFY] Files for '%s' missing on flash!\n", id.c_str());
            return false;
        }
    }

    File vf = LittleFS.open(vPath, FILE_READ);
    File af = LittleFS.open(aPath, FILE_READ);
    if (!vf || !af) {
        Serial.printf("[VERIFY] Failed to open files for '%s' on flash.\n", id.c_str());
        if (vf) vf.close();
        if (af) af.close();
        return false;
    }

    size_t actualV = vf.size();
    size_t actualA = af.size();

    // Verify STIK magic header
    uint8_t header[16];
    size_t hRead = vf.read(header, 16);
    vf.close();
    af.close();

    if (hRead < 16 || header[0] != 'S' || header[1] != 'T' || header[2] != 'I' || header[3] != 'K') {
        Serial.printf("[VERIFY] Video '%s' failed STIK header validation!\n", vPath.c_str());
        return false;
    }

    if (expectedVideoSize > 0 && actualV != expectedVideoSize) {
        Serial.printf("[VERIFY] Video size mismatch! Expected %u, got %u\n",
                      (unsigned)expectedVideoSize, (unsigned)actualV);
        return false;
    }

    if (expectedAudioSize > 0 && actualA != expectedAudioSize) {
        Serial.printf("[VERIFY] Audio size mismatch! Expected %u, got %u\n",
                      (unsigned)expectedAudioSize, (unsigned)actualA);
        return false;
    }

    Serial.printf("[VERIFY] Verified offline media for '%s' (Video: %u, Audio: %u bytes)\n",
                  id.c_str(), (unsigned)actualV, (unsigned)actualA);
    return true;
}

bool OfflineStorage::rebuildCatalogueFromDisk() {
    if (!m_mounted) return false;

    Serial.println("[STORAGE] Rebuilding offline catalogue from actual flash contents...");
    m_offlineCatalogue.items.clear();
    m_offlineCatalogue.page = 0;
    m_offlineCatalogue.total = 0;

    // Scan /offline directory
    File offDir = LittleFS.open(OFFLINE_DIR);
    if (offDir && offDir.isDirectory()) {
        File sub = offDir.openNextFile();
        while (sub) {
            if (sub.isDirectory()) {
                String subPath = sub.path(); // e.g. /offline/v0006
                String id = sub.name();      // e.g. v0006
                String vPath = subPath + "/video.bin";
                String aPath = subPath + "/audio.bin";

                if (LittleFS.exists(vPath) && LittleFS.exists(aPath)) {
                    File vf = LittleFS.open(vPath, FILE_READ);
                    File af = LittleFS.open(aPath, FILE_READ);
                    if (vf && af && vf.size() >= 16 && af.size() > 0) {
                        uint8_t header[16];
                        size_t hRead = vf.read(header, 16);
                        if (hRead == 16 && header[0] == 'S' && header[1] == 'T' && header[2] == 'I' && header[3] == 'K') {
                            MediaItem item;
                            item.id = id;
                            item.name = "Offline " + id;
                            item.videoUrl = vPath;
                            item.audioUrl = aPath;
                            item.isSpiffsCompatible = true;
                            item.isOfflineStored = true;
                            item.totalSize = vf.size() + af.size();
                            m_offlineCatalogue.items.push_back(item);
                            Serial.printf("[STORAGE] Discovered valid offline item: %s (%u bytes)\n",
                                          id.c_str(), (unsigned)item.totalSize);
                        }
                    }
                    if (vf) vf.close();
                    if (af) af.close();
                }
            }
            sub = offDir.openNextFile();
        }
        offDir.close();
    }

    // Also check root for legacy flat files /off_<id>_v.bin
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            String name = f.name();
            if (name.startsWith("off_") && name.endsWith("_v.bin")) {
                // e.g. off_v0006_v.bin -> id is v0006
                String id = name.substring(4, name.length() - 6);
                String vPath = "/off_" + id + "_v.bin";
                String aPath = "/off_" + id + "_a.bin";
                if (LittleFS.exists(aPath)) {
                    bool alreadyAdded = false;
                    for (const auto& ex : m_offlineCatalogue.items) {
                        if (ex.id == id) { alreadyAdded = true; break; }
                    }
                    if (!alreadyAdded) {
                        File vf = LittleFS.open(vPath, FILE_READ);
                        File af = LittleFS.open(aPath, FILE_READ);
                        if (vf && af && vf.size() >= 16 && af.size() > 0) {
                            MediaItem item;
                            item.id = id;
                            item.name = "Offline " + id;
                            item.videoUrl = vPath;
                            item.audioUrl = aPath;
                            item.isSpiffsCompatible = true;
                            item.isOfflineStored = true;
                            item.totalSize = vf.size() + af.size();
                            m_offlineCatalogue.items.push_back(item);
                            Serial.printf("[STORAGE] Discovered legacy offline item: %s (%u bytes)\n",
                                          id.c_str(), (unsigned)item.totalSize);
                        }
                        if (vf) vf.close();
                        if (af) af.close();
                    }
                }
            }
            f = root.openNextFile();
        }
        root.close();
    }

    m_offlineCatalogue.total = m_offlineCatalogue.items.size();
    if (m_offlineCatalogue.total > 0) {
        saveCatalogueToFlash();
        Serial.printf("[STORAGE] Rebuild complete: Saved %u items to offline catalogue.\n", (unsigned)m_offlineCatalogue.total);
        return true;
    }

    Serial.println("[STORAGE] Rebuild complete: No valid media files found on flash.");
    return false;
}

bool OfflineStorage::loadItemToPSRAM(const MediaItem& item, DownloadedMedia& outResult) {
    if (!m_mounted) {
        Serial.println("[STORAGE] LittleFS not mounted.");
        return false;
    }

    // Resolve paths on flash:
    // 1. /offline/<id>/video.bin & audio.bin
    // 2. /off_<id>_v.bin & /off_<id>_a.bin
    // 3. item.videoUrl & item.audioUrl (if they exist as local paths on flash)
    String vPath = getVideoFilePath(item.id);
    String aPath = getAudioFilePath(item.id);

    if (!LittleFS.exists(vPath) || !LittleFS.exists(aPath)) {
        String legV = getLegacyVideoFilePath(item.id);
        String legA = getLegacyAudioFilePath(item.id);
        if (LittleFS.exists(legV) && LittleFS.exists(legA)) {
            vPath = legV;
            aPath = legA;
        } else if (item.videoUrl.length() > 0 && LittleFS.exists(item.videoUrl) &&
                   item.audioUrl.length() > 0 && LittleFS.exists(item.audioUrl)) {
            vPath = item.videoUrl;
            aPath = item.audioUrl;
        } else {
            Serial.printf("[STORAGE] Error: Offline media files for '%s' not found on flash!\n", item.id.c_str());
            Serial.printf("          Checked: %s and %s\n", vPath.c_str(), legV.c_str());
            return false;
        }
    }

    File vFile = LittleFS.open(vPath, FILE_READ);
    if (!vFile) {
        Serial.printf("[STORAGE] Failed to open flash video file: %s\n", vPath.c_str());
        return false;
    }

    size_t vSize = vFile.size();
    vFile.close();

    DownloadedMedia temp;
    temp.item = item;
    temp.item.isOfflineStored = true;
    temp.videoFilePath = vPath;
    temp.videoSize = vSize;
    temp.videoBuffer = nullptr;

    if (psramFound() && LittleFS.exists(aPath)) {
        File aFile = LittleFS.open(aPath, FILE_READ);
        if (aFile) {
            size_t aSize = aFile.size();
            size_t freePsram = ESP.getFreePsram();
            if (aSize > 0 && freePsram >= (aSize + PSRAM_SAFETY_MARGIN_BYTES)) {
                temp.audioBuffer = (uint8_t*)ps_malloc(aSize);
                if (temp.audioBuffer) {
                    size_t aRead = aFile.read(temp.audioBuffer, aSize);
                    if (aRead == aSize) {
                        temp.audioSize = aSize;
                        temp.audioFilePath = aPath;
                        Serial.printf("[STORAGE] Audio loaded into PSRAM (%u bytes).\n", (unsigned)aSize);
                    } else {
                        free(temp.audioBuffer);
                        temp.audioBuffer = nullptr;
                        temp.audioSize = 0;
                    }
                }
            }
            aFile.close();
        }
    } else if (LittleFS.exists(aPath)) {
        File aFile = LittleFS.open(aPath, FILE_READ);
        if (aFile) {
            size_t aSize = aFile.size();
            aFile.close();
            if (aSize > 0) {
                temp.audioFilePath = aPath;
                temp.audioSize = aSize;
                Serial.printf("[STORAGE] WROOM-32: Streaming video & audio (%u bytes) directly from SPIFFS.\n", (unsigned)aSize);
            }
        }
        temp.audioBuffer = nullptr;
    } else {
        temp.audioBuffer = nullptr;
        temp.audioSize = 0;
    }

    outResult = temp;
    Serial.printf("[STORAGE] Offline media '%s' ready from SPIFFS! (Video: %u bytes, Audio: %u bytes)\n",
                  item.name.c_str(), (unsigned)vSize, (unsigned)temp.audioSize);
    return true;
}

bool OfflineStorage::manualFormat() {
    Serial.println("[FS] Manual format requested via command...");
    m_offlineCatalogue.items.clear();
    m_offlineCatalogue.total = 0;
    bool ok = LittleFS.format();
    if (ok) {
        Serial.println("[FS] Format SUCCESS. Remounting...");
        m_mounted = LittleFS.begin(false, "/littlefs", 10, "spiffs");
        if (m_mounted) {
            LittleFS.mkdir(OFFLINE_DIR);
            printDiagnostics();
        }
    } else {
        Serial.println("[FS] Format FAILED!");
    }
    return ok;
}
