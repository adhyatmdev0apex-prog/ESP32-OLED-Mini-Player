#include "playback_manager.h"
#include "../transport/command_protocol.h"
#include "../input/input_manager.h"

struct BluetoothPlaybackSnapshot {
    bool isConnected;
    bool isStreaming;
    char deviceName[64];
};

static void captureBTSnapshot(BluetoothPlaybackSnapshot* s) {
    s->isConnected = bluetoothAudioConnected();
    s->isStreaming = bluetoothAudioStreaming();
    const char* dev = bluetoothAudioConnectedDeviceName();
    strncpy(s->deviceName, dev ? dev : "", sizeof(s->deviceName) - 1);
    s->deviceName[sizeof(s->deviceName) - 1] = '\0';
}

static bool btSnapshotChanged(const BluetoothPlaybackSnapshot& s) {
    bool nowConnected = bluetoothAudioConnected();
    bool nowStreaming = bluetoothAudioStreaming();
    if (nowConnected != s.isConnected) return true;
    if (nowStreaming != s.isStreaming) return true;
    if (nowConnected) {
        const char* dev = bluetoothAudioConnectedDeviceName();
        return strcmp(s.deviceName, dev ? dev : "") != 0;
    }
    return false;
}

PlaybackManager& PlaybackManager::getInstance() {
    static PlaybackManager instance;
    return instance;
}

PlaybackManager::PlaybackManager()
    : m_isLoaded(false)
    , m_state(PlaybackState::STOPPED)
    , m_width(128)
    , m_height(64)
    , m_fps(29)
    , m_frameCount(0)
    , m_frameBuffer(nullptr)
    , m_deltaBuffer(nullptr)
    , m_rleBuffer(nullptr)
    , m_audioRingBuffer(nullptr)
    , m_inPlayLoop(false)
    , m_restartRequested(false) {
}

void PlaybackManager::begin() {
    if (psramFound()) {
        m_frameBuffer     = (uint8_t*)ps_malloc(1024);
        m_deltaBuffer     = (uint8_t*)ps_malloc(1024);
        m_rleBuffer       = (uint8_t*)ps_malloc(1040);
        m_audioRingBuffer = (uint8_t*)ps_malloc(AUDIO_RING_SIZE);
        if (m_audioRingBuffer) {
            memset(m_audioRingBuffer, 128, AUDIO_RING_SIZE);
        }
    } else {
        m_frameBuffer     = (uint8_t*)malloc(1024);
        m_deltaBuffer     = (uint8_t*)malloc(1024);
        m_rleBuffer       = (uint8_t*)malloc(1040);
        m_audioRingBuffer = (uint8_t*)malloc(AUDIO_RING_SIZE);
        if (m_audioRingBuffer) {
            memset(m_audioRingBuffer, 128, AUDIO_RING_SIZE);
        }
    }

    if (!m_frameBuffer || !m_deltaBuffer || !m_rleBuffer) {
        Serial.println("[PLAYBACK] Error: Frame buffer allocation failed!");
    } else {
        memset(m_frameBuffer, 0, 1024);
        memset(m_deltaBuffer, 0, 1024);
        memset(m_rleBuffer, 0, 1040);
    }
}

bool PlaybackManager::setMedia(DownloadedMedia& newMedia) {
    bool valid = false;

    if (newMedia.videoFilePath.length() > 0) {
        if (!LittleFS.exists(newMedia.videoFilePath)) {
            Serial.printf("[PLAYBACK] Error: Video file '%s' not found on SPIFFS.\n", newMedia.videoFilePath.c_str());
            return false;
        }
        File f = LittleFS.open(newMedia.videoFilePath, FILE_READ);
        if (!f) {
            Serial.printf("[PLAYBACK] Error: Cannot open video file '%s'.\n", newMedia.videoFilePath.c_str());
            return false;
        }
        valid = parseSTIKHeaderFromFile(f);
        f.close();
        if (!valid) {
            Serial.println("[PLAYBACK] Error: Failed to parse STIK header from file.");
            return false;
        }
    } else if (newMedia.videoBuffer != nullptr && newMedia.videoSize >= 16) {
        valid = parseSTIKHeader(newMedia.videoBuffer, newMedia.videoSize);
        if (!valid) {
            Serial.println("[PLAYBACK] Error: Failed to parse STIK header from memory.");
            return false;
        }
    } else {
        Serial.println("[PLAYBACK] Error: Cannot set empty video source.");
        return false;
    }

    // Stop playback if currently active
    stop();

    // Free previously loaded media buffers in PSRAM
    m_currentMedia.freeBuffers();

    // Swap ownership of new media
    m_currentMedia = newMedia;
    newMedia.videoBuffer = nullptr;
    newMedia.audioBuffer = nullptr;
    newMedia.videoSize = 0;
    newMedia.audioSize = 0;
    newMedia.videoFilePath = "";
    newMedia.audioFilePath = "";

    // Attach audio PCM buffer (or SPIFFS ring buffer) to Bluetooth Classic A2DP source
    if (m_currentMedia.audioBuffer && m_currentMedia.audioSize > 0) {
        bluetoothAudioPause(); // Ensure audio is paused until playCurrent() starts
        bluetoothAudioSetPCMRingMask(0xFFFFFFFFUL);
        bluetoothAudioSetPCMBuffer(m_currentMedia.audioBuffer, m_currentMedia.audioSize, AUDIO_SAMPLE_RATE);
    } else if (m_currentMedia.audioFilePath.length() > 0 && m_currentMedia.audioSize > 0 && m_audioRingBuffer != nullptr) {
        bluetoothAudioPause();
        memset(m_audioRingBuffer, 128, AUDIO_RING_SIZE);
        bluetoothAudioSetPCMRingMask(AUDIO_RING_SIZE - 1);
        bluetoothAudioSetPCMBuffer(m_audioRingBuffer, m_currentMedia.audioSize, AUDIO_SAMPLE_RATE);
    } else {
        bluetoothAudioSetPCMRingMask(0xFFFFFFFFUL);
        bluetoothAudioSetPCMBuffer(nullptr, 0, AUDIO_SAMPLE_RATE);
    }

    m_isLoaded = true;
    if (m_currentMedia.videoFilePath.length() > 0) {
        Serial.printf("[PLAYBACK] Media '%s' loaded from SPIFFS! (%s, %u frames @ %u fps, Audio: %u bytes)\n",
                      m_currentMedia.item.name.c_str(), m_currentMedia.videoFilePath.c_str(),
                      (unsigned)m_frameCount, (unsigned)m_fps, (unsigned)m_currentMedia.audioSize);
    } else {
        Serial.printf("[PLAYBACK] Media '%s' loaded into memory! (%u frames @ %u fps)\n",
                      m_currentMedia.item.name.c_str(), (unsigned)m_frameCount, (unsigned)m_fps);
    }
    return true;
}

bool PlaybackManager::loadFallbackLittleFS() {
    if (!LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
        Serial.println("[PLAYBACK] LittleFS not available for fallback.");
        return false;
    }

    const char* vPath = VIDEO_FILE;
    const char* aPath = AUDIO_FILE;
    if (!LittleFS.exists(vPath)) {
        if (LittleFS.exists("/stream_video.bin")) {
            vPath = "/stream_video.bin";
            aPath = "/stream_audio.bin";
        } else {
            Serial.println("[PLAYBACK] Fallback video file not found in LittleFS.");
            return false;
        }
    }

    File vid = LittleFS.open(vPath, FILE_READ);
    if (!vid) return false;
    size_t vidSize = vid.size();
    vid.close();

    DownloadedMedia fallback;
    fallback.item.id = "local_fallback";
    fallback.item.name = "Local LittleFS Fallback";
    fallback.item.videoUrl = vPath;
    fallback.videoFilePath = vPath;
    fallback.videoSize = vidSize;
    fallback.videoBuffer = nullptr;

    if (LittleFS.exists(aPath)) {
        File aud = LittleFS.open(aPath, FILE_READ);
        if (aud) {
            size_t audSize = aud.size();
            fallback.item.audioUrl = aPath;
            fallback.audioFilePath = aPath;
            if (psramFound() && audSize > 0) {
                fallback.audioBuffer = (uint8_t*)ps_malloc(audSize);
                if (fallback.audioBuffer) {
                    aud.read(fallback.audioBuffer, audSize);
                    fallback.audioSize = audSize;
                }
            } else {
                fallback.audioBuffer = nullptr;
                fallback.audioSize = audSize;
            }
            aud.close();
        }
    }

    return setMedia(fallback);
}

bool PlaybackManager::parseSTIKHeader(const uint8_t* data, size_t size) {
    if (!data || size < 16) return false;
    if (data[0] != 'S' || data[1] != 'T' || data[2] != 'I' || data[3] != 'K') return false;

    m_width = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    m_height = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    m_fps = data[8];
    m_frameCount = (uint32_t)data[9] | ((uint32_t)data[10] << 8) |
                   ((uint32_t)data[11] << 16) | ((uint32_t)data[12] << 24);

    return m_width == 128 && m_height == 64 && m_fps > 0 && m_frameCount > 0;
}

bool PlaybackManager::parseSTIKHeaderFromFile(File& file) {
    if (!file || file.size() < 16) return false;
    uint8_t header[16];
    size_t n = file.read(header, 16);
    if (n != 16) return false;
    return parseSTIKHeader(header, 16);
}

bool PlaybackManager::decodeRLE(const uint8_t* payload, size_t payloadLen, uint8_t* output) {
    size_t inIdx = 0;
    size_t outIdx = 0;

    while (inIdx < payloadLen && outIdx < 1024) {
        uint8_t ctrl = payload[inIdx++];
        if ((ctrl & 0x80) == 0) {
            // ZERO RUN
            size_t count = (size_t)ctrl + 1;
            for (size_t k = 0; k < count && outIdx < 1024; k++) {
                output[outIdx++] = 0;
            }
        } else {
            // LITERAL RUN
            size_t count = (size_t)(ctrl & 0x7F) + 1;
            for (size_t k = 0; k < count && inIdx < payloadLen && outIdx < 1024; k++) {
                output[outIdx++] = payload[inIdx++];
            }
        }
    }
    return outIdx == 1024;
}

bool PlaybackManager::readNextFrame(const uint8_t* data, size_t size, size_t& offset) {
    if (!data || offset >= size) return false;

    uint8_t type = data[offset++];
    if (offset + 2 > size) return false;

    uint16_t payloadLen = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
    offset += 2;

    if (offset + payloadLen > size) return false;

    if (type == 0x01) {
        // RAW FRAME (1024 bytes)
        if (payloadLen != 1024) return false;
        memcpy(m_frameBuffer, data + offset, 1024);
        offset += 1024;
        return true;
    } else if (type == 0x02) {
        // XOR-RLE DELTA
        if (!decodeRLE(data + offset, payloadLen, m_deltaBuffer)) {
            return false;
        }
        offset += payloadLen;
        for (int i = 0; i < 1024; i++) {
            m_frameBuffer[i] ^= m_deltaBuffer[i];
        }
        return true;
    }

    return false;
}

bool PlaybackManager::readNextFrameFromFile(File& file) {
    if (!file || !file.available()) return false;

    int typeByte = file.read();
    if (typeByte < 0) return false;
    uint8_t type = (uint8_t)typeByte;

    uint8_t lenBuf[2];
    if (file.read(lenBuf, 2) != 2) return false;
    uint16_t payloadLen = (uint16_t)lenBuf[0] | ((uint16_t)lenBuf[1] << 8);

    if (type == 0x01) {
        // RAW FRAME (1024 bytes)
        if (payloadLen != 1024) {
            Serial.printf("[PLAYBACK] Malformed raw frame record: expected 1024 bytes, got %u\n", payloadLen);
            return false;
        }
        size_t n = file.read(m_frameBuffer, 1024);
        if (n != 1024) {
            Serial.printf("[PLAYBACK] Truncated raw frame: expected 1024 bytes, read %u\n", (unsigned)n);
            return false;
        }
        return true;
    } else if (type == 0x02) {
        // XOR-RLE DELTA
        if (payloadLen == 0 || payloadLen > 1040 || !m_rleBuffer) {
            Serial.printf("[PLAYBACK] Malformed delta record: payload length %u invalid\n", payloadLen);
            return false;
        }
        size_t n = file.read(m_rleBuffer, payloadLen);
        if (n != payloadLen) {
            Serial.printf("[PLAYBACK] Truncated delta record: expected %u bytes, read %u\n", payloadLen, (unsigned)n);
            return false;
        }

        if (!decodeRLE(m_rleBuffer, payloadLen, m_deltaBuffer)) {
            Serial.println("[PLAYBACK] RLE decode failure in delta frame.");
            return false;
        }
        for (int i = 0; i < 1024; i++) {
            m_frameBuffer[i] ^= m_deltaBuffer[i];
        }
        return true;
    }

    Serial.printf("[PLAYBACK] Unknown record type: 0x%02X\n", type);
    return false;
}

void PlaybackManager::stop() {
    m_state = PlaybackState::STOPPED;
    if (m_vFile) m_vFile.close();
    if (m_aFile) m_aFile.close();
    bluetoothAudioPause();
    bluetoothAudioSetTime(0);
}

void PlaybackManager::pause() {
    if (m_state == PlaybackState::PLAYING) {
        m_state = PlaybackState::PAUSED;
        bluetoothAudioPause();
    }
}

void PlaybackManager::resume() {
    if (m_state == PlaybackState::PAUSED) {
        m_state = PlaybackState::PLAYING;
        bluetoothAudioResume();
    }
}

void PlaybackManager::freeCurrentMedia() {
    stop();
    m_currentMedia.freeBuffers();
    m_isLoaded = false;
    bluetoothAudioSetPCMRingMask(0xFFFFFFFFUL);
    bluetoothAudioSetPCMBuffer(nullptr, 0, AUDIO_SAMPLE_RATE);
    Serial.println("[PLAYBACK] Current media resources released.");
}

void PlaybackManager::playCurrent() {
    if (!m_isLoaded) {
        Serial.println("[PLAYBACK] Cannot play: No media loaded.");
        return;
    }

    if (m_inPlayLoop) {
        // Triggered from pollSerial() while already inside playCurrent() loop:
        // signal outer loop to restart cleanly without nesting stack frames.
        m_restartRequested = true;
        m_state = PlaybackState::PLAYING;
        return;
    }

    if (m_state == PlaybackState::PLAYING) {
        Serial.println("[PLAYBACK] Already playing. Use 'stop' or 'pause' first.");
        return;
    }

    m_inPlayLoop = true;
    struct PlayLoopGuard {
        bool& inLoop;
        ~PlayLoopGuard() { inLoop = false; }
    } loopGuard{m_inPlayLoop};

startNewMedia:
    m_restartRequested = false;

    bool usingFile = (m_currentMedia.videoFilePath.length() > 0);
    if (!usingFile && m_currentMedia.videoBuffer == nullptr) {
        Serial.println("[PLAYBACK] Cannot play: No video source available.");
        m_state = PlaybackState::STOPPED;
        return;
    }

    bool hasMemoryAudio = (m_currentMedia.audioBuffer != nullptr && m_currentMedia.audioSize > 0);
    bool hasFileAudio = (!hasMemoryAudio &&
                         m_currentMedia.audioFilePath.length() > 0 &&
                         m_currentMedia.audioSize > 0 &&
                         m_audioRingBuffer != nullptr);

    m_state = PlaybackState::PLAYING;
    OLEDDisplay::getInstance().showPlayingScreen(m_currentMedia.item.name.c_str());

    const uint32_t fps = (m_fps > 0 ? m_fps : 24);
    const uint32_t frameTimeUs = 1000000UL / fps;

    if (m_vFile) m_vFile.close();
    if (usingFile) {
        m_vFile = LittleFS.open(m_currentMedia.videoFilePath, FILE_READ);
        if (!m_vFile || m_vFile.size() < 16) {
            Serial.printf("[PLAYBACK] Failed to open '%s' for playback.\n", m_currentMedia.videoFilePath.c_str());
            if (m_vFile) m_vFile.close();
            m_state = PlaybackState::STOPPED;
            return;
        }
    }

    if (m_aFile) m_aFile.close();
    if (hasFileAudio) {
        m_aFile = LittleFS.open(m_currentMedia.audioFilePath, FILE_READ);
        if (!m_aFile || m_aFile.size() == 0) {
            Serial.printf("[PLAYBACK] Warning: Could not open audio file '%s'. Continuing without audio.\n",
                          m_currentMedia.audioFilePath.c_str());
            if (m_aFile) m_aFile.close();
            hasFileAudio = false;
            bluetoothAudioSetPCMRingMask(0xFFFFFFFFUL);
            bluetoothAudioSetPCMBuffer(nullptr, 0, AUDIO_SAMPLE_RATE);
        }
    }

    bool hasAudio = (hasMemoryAudio || hasFileAudio);
    size_t audioRingLoaded = 0;

restartActualAnimation:
    bluetoothAudioUpdate();
    BluetoothPlaybackSnapshot bluetoothAtStart;
    captureBTSnapshot(&bluetoothAtStart);

    size_t memOffset = 16; // Skip 16-byte STIK header
    if (usingFile && m_vFile) {
        m_vFile.seek(16); // Rewind to first frame
    }
    memset(m_frameBuffer, 0, 1024);

    if (hasFileAudio && m_aFile) {
        bluetoothAudioPause();
        memset(m_audioRingBuffer, 128, AUDIO_RING_SIZE);
        m_aFile.seek(0);
        size_t initialBytes = std::min((size_t)AUDIO_RING_SIZE, m_currentMedia.audioSize);
        audioRingLoaded = m_aFile.read(m_audioRingBuffer, initialBytes);
    }

    uint32_t playbackStartUs = micros();
    if (hasAudio) {
        bluetoothAudioSetTime(0); // Snap audio position strictly to start
        bluetoothAudioResume();
    }

    uint32_t actualFrames = 0;

    for (uint32_t frame = 0; frame < m_frameCount; frame++) {
        if (m_restartRequested) {
            if (m_vFile) m_vFile.close();
            if (m_aFile) m_aFile.close();
            goto startNewMedia;
        }

        if (m_state == PlaybackState::STOPPED) {
            Serial.println("[PLAYBACK] Playback stopped by user.");
            break;
        }

        if (m_state == PlaybackState::PAUSED) {
            bluetoothAudioPause();
            uint32_t pauseStartMs = millis();
            OLEDDisplay::getInstance().showPauseScreen(m_currentMedia.item.name.c_str());

            while (m_state == PlaybackState::PAUSED) {
                delay(20);
                bluetoothAudioUpdate();
                CommandProtocol::getInstance().pollSerial();
                if (m_restartRequested) break;

                InputEvent pEv = InputManager::getInstance().poll();
                if (pEv == InputEvent::SELECT_SHORT_PRESS) {
                    resume();
                    break;
                } else if (pEv == InputEvent::SELECT_LONG_PRESS) {
                    Serial.println("[INPUT] Long press while paused -> Stopping playback.");
                    stop();
                    break;
                }
            }

            if (m_restartRequested) {
                if (m_vFile) m_vFile.close();
                if (m_aFile) m_aFile.close();
                goto startNewMedia;
            }
            if (m_state == PlaybackState::STOPPED) break;

            // Compensate frame clocks for the duration of the pause
            uint32_t pausedUs = (millis() - pauseStartMs) * 1000UL;
            playbackStartUs += pausedUs;
            if (hasAudio) bluetoothAudioResume();
        }

        // Bluetooth snapshot sync check (Requirement #21)
        bluetoothAudioUpdate();
        if (btSnapshotChanged(bluetoothAtStart)) {
            Serial.println("BT STATE/DEVICE CHANGED - RESTARTING ANIMATION");
            if (hasAudio) {
                bluetoothAudioSetTime(0);
            }
            goto restartActualAnimation;
        }

        // Presentation timestamp of this video frame (PTS in milliseconds)
        uint32_t framePtsMs = (uint32_t)(((uint64_t)frame * 1000ULL) / fps);

        // Refill SPIFFS audio ring buffer ahead of current presentation clock
        if (hasFileAudio && m_aFile) {
            uint32_t curSample = (uint32_t)(((uint64_t)framePtsMs * AUDIO_SAMPLE_RATE) / 1000ULL);
            size_t targetLoaded = std::min(m_currentMedia.audioSize, (size_t)curSample + (AUDIO_RING_SIZE - 2048));
            while (audioRingLoaded < targetLoaded) {
                size_t ringWritePos = audioRingLoaded & (AUDIO_RING_SIZE - 1);
                size_t contig = AUDIO_RING_SIZE - ringWritePos;
                size_t toRead = std::min(contig, std::min((size_t)1024, targetLoaded - audioRingLoaded));
                size_t r = m_aFile.read(m_audioRingBuffer + ringWritePos, toRead);
                if (r == 0) break;
                audioRingLoaded += r;
            }
        }

        // Audio clock update: lock audio strictly to current video presentation timestamp
        if (hasAudio) {
            bluetoothAudioSetTime(framePtsMs);
        }

        // Poll physical buttons before frame decode
        InputEvent preEv = InputManager::getInstance().poll();
        if (preEv == InputEvent::SELECT_SHORT_PRESS) {
            pause();
            continue;
        } else if (preEv == InputEvent::SELECT_LONG_PRESS) {
            Serial.println("[INPUT] Long press (hold 1s) -> Stopping playback.");
            stop();
            break;
        }

        // Read STIK frame
        bool frameOk = false;
        if (usingFile) {
            frameOk = readNextFrameFromFile(m_vFile);
        } else {
            frameOk = readNextFrame(m_currentMedia.videoBuffer, m_currentMedia.videoSize, memOffset);
        }

        if (!frameOk) {
            Serial.printf("\n[PLAYBACK] Reached end of video stream at frame %lu / %lu.\n",
                          (unsigned long)frame, (unsigned long)m_frameCount);
            break;
        }

        // FAST native 128x64 OLED rendering (13ms transfer, no 24-byte packet fragmentation)
        OLEDDisplay::getInstance().renderFrame(m_frameBuffer);

        actualFrames++;

        // Target presentation timestamp for NEXT frame
        uint32_t targetNextUs = playbackStartUs + (uint32_t)(((uint64_t)(frame + 1) * 1000000ULL) / fps);

        // Lag compensation: if rendering fell behind target by > 2 frames (>70ms),
        // re-anchor playbackStartUs to avoid runaway cumulative delay:
        int32_t lagUs = (int32_t)(micros() - targetNextUs);
        if (lagUs > (int32_t)(frameTimeUs * 2)) {
            playbackStartUs = micros() - (uint32_t)(((uint64_t)(frame + 1) * 1000000ULL) / fps);
            targetNextUs = micros();
        }

        // High-precision sub-millisecond wait loop without FreeRTOS tick overshoot
        while ((int32_t)(micros() - targetNextUs) < 0) {
            int32_t remainingUs = (int32_t)(targetNextUs - micros());
            if (remainingUs > 2500) {
                delay(1); // Sleep 1 FreeRTOS tick when enough time exists
            } else if (remainingUs > 50) {
                delayMicroseconds(50); // Microsecond precision for the final fraction
            } else {
                taskYIELD();
            }

            bluetoothAudioUpdate();
            if (btSnapshotChanged(bluetoothAtStart)) {
                Serial.println("BT STATE/DEVICE CHANGED - RESTARTING ANIMATION");
                if (hasAudio) {
                    bluetoothAudioSetTime(0);
                }
                goto restartActualAnimation;
            }

            // Poll physical buttons during wait
            InputEvent btnEv = InputManager::getInstance().poll();
            if (btnEv == InputEvent::SELECT_SHORT_PRESS) {
                pause();
                break;
            } else if (btnEv == InputEvent::SELECT_LONG_PRESS) {
                Serial.println("[INPUT] Long press (hold 1s) -> Stopping playback.");
                stop();
                break;
            }

            CommandProtocol::getInstance().pollSerial();
            if (m_restartRequested || m_state != PlaybackState::PLAYING) break;
        }
    }

    if (m_vFile) m_vFile.close();
    if (m_aFile) m_aFile.close();

    if (m_restartRequested) {
        goto startNewMedia;
    }

    if (hasAudio) {
        bluetoothAudioPause();
        bluetoothAudioSetTime(0);
    }

    m_state = PlaybackState::STOPPED;
    Serial.printf("[PLAYBACK] Video finished (%lu frames played).\n", (unsigned long)actualFrames);
}

void PlaybackManager::update() {
    bluetoothAudioUpdate();
}
