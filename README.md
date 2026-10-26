# OLED Mini Player

**A tiny ESP32-powered animation player with a 128×64 OLED, Bluetooth audio, Wi-Fi media delivery, and offline storage.**

OLED Mini Player turns compatible ESP32 boards into a pocket-sized media player. It renders monochrome animations on an I²C OLED and sends matching audio to a Bluetooth speaker or headphones. Media can come from the companion web server, the device's offline library, or bundled fallback files.

> **Project status:** This README documents the existing project structure and behavior. Hardware-specific pinouts, memory limits, and available features depend on the selected PlatformIO environment and the board you actually use.

---

## Contents

- [Highlights](#highlights)
- [How it works](#how-it-works)
- [Hardware and wiring](#hardware-and-wiring)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Configure the firmware](#configure-the-firmware)
- [Build and flash](#build-and-flash)
- [Run the web server](#run-the-web-server)
- [Convert and publish media](#convert-and-publish-media)
- [Operate the player](#operate-the-player)
- [Offline storage](#offline-storage)
- [Serial command reference](#serial-command-reference)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [Security notes](#security-notes)

---

## Highlights

- **128×64 monochrome animation playback** on an I²C OLED.
- **Bluetooth Classic A2DP audio** for a compatible speaker or headphones.
- **Wi-Fi catalogue browsing** from a companion Node.js media server.
- **Media management in a browser** for adding, replacing, editing, and deleting catalogue entries.
- **Offline downloads** for media marked as compatible and within the configured size limit.
- **Local fallback assets** bundled into the device filesystem.
- **Serial diagnostics and controls** for Wi-Fi, media, filesystem, and Bluetooth.
- **Multiple PlatformIO targets** for standard ESP32, PSRAM-equipped ESP32, and ESP32-CAM boards.

### Memory matters

The standard ESP32-WROOM target has **no PSRAM**. According to the current firmware configuration, remote media playback/downloads that require loading the media pair into PSRAM are rejected when PSRAM is unavailable. The no-PSRAM target is intended for compatible local filesystem media that fits the available storage and RAM. Do not assume that a large online media item will work on a WROOM simply because it fits in flash.

---

## How it works

1. The firmware initializes the display, buttons, playback engine, filesystem, and Wi-Fi manager.
2. It tries the primary Wi-Fi profile, followed by the optional fallback profile.
3. When online, it requests a paginated JSON catalogue from the configured media server.
4. Selecting an online item downloads its video and audio files into PSRAM on supported targets.
5. The video is decoded frame-by-frame and drawn on the OLED; audio is sent over Bluetooth Classic A2DP.
6. Media marked for offline use can be saved to the device filesystem, subject to the configured limit and available space.
7. If neither Wi-Fi profile connects, the firmware attempts to open its offline library. Configured local fallback files are a separate playback path.

Related documentation:

- [Web server and browser UI guide](WEB/README.md)
- [Media server API contract](WEB/API.md)

---

## Hardware and wiring

### Supported build targets

| PlatformIO environment | Intended board | Memory notes |
|---|---|---|
| `esp32dev` | Standard ESP32 development board, such as ESP32-WROOM-32 | No PSRAM assumed; use media compatible with local storage and available RAM |
| `esp32dev_psram` | ESP32 development board with PSRAM | PSRAM-dependent media workflows may be available |
| `esp32cam` | ESP32-CAM | Board pin availability and memory configuration differ from standard ESP32 boards |

Select the environment that matches the **actual hardware**. A build succeeding for one environment does not prove that another board has the same memory or pin capabilities.

### Required components

- ESP32 development board or ESP32-CAM.
- 128×64 SSD1306-compatible I²C OLED.
- Two momentary push buttons.
- Bluetooth Classic A2DP receiver, such as a compatible speaker or headphones.
- Data-capable USB cable and any required USB-to-serial adapter.

### Current firmware pin assignments

| Component | ESP32 GPIO | Wiring |
|---|---:|---|
| OLED SDA | GPIO 13 | OLED SDA |
| OLED SCL | GPIO 14 | OLED SCL |
| Scroll button | GPIO 4 | Button between GPIO 4 and GND |
| Select button | GPIO 5 | Button between GPIO 5 and GND |
| OLED power and ground | Board/module dependent | Follow the OLED module's rated voltage; connect grounds together |

The buttons use internal pull-ups and are **active-low**: pressing a button connects its GPIO to GND.

> **Check your board before wiring.** GPIO 4 and GPIO 5 may be unavailable or shared with other functions on some boards. GPIO 5 can also be a boot-strapping pin. ESP32-CAM boards have additional pin constraints. If necessary, update the shared pin definitions in `src/config/app_config.h` and verify the selected board's pinout.

---

## Requirements

### Firmware

- PlatformIO Core/CLI or the PlatformIO IDE extension.
- USB serial drivers and a data-capable USB cable.
- A 2.4 GHz Wi-Fi network for online catalogue access.
- A compatible Bluetooth Classic A2DP receiver for audio playback.

The firmware uses the Arduino framework. Dependencies are declared in `platformio.ini`, including U8g2, ESP32-A2DP, and ArduinoJson.

### Web server

- Node.js 18 or later.
- The current server does not declare third-party npm packages.

### Media conversion

- Python 3.
- OpenCV and NumPy for video conversion.
- FFmpeg installed and available on `PATH` for audio conversion.

Install the Python dependencies:

```powershell
py -m pip install opencv-python numpy
```

Check that FFmpeg is available in a new terminal:

```powershell
ffmpeg -version
```

---

## Quick start

### 1. Get the project ready

Open a terminal in the repository root—the directory containing `platformio.ini`.

### 2. Configure Wi-Fi

Edit `src/config/app_config.h` and set your local Wi-Fi credentials. Keep real credentials out of public commits.

### 3. Choose the media server

The checked-in `MEDIA_SERVER_BASE_URL` currently points to the hosted instance:

```text
https://oled-mini-player.onrender.com
```

Leave it as configured to use that instance, or set it to your own server's reachable LAN address, for example:

```text
http://192.168.1.20:3000
```

Do **not** use `localhost` or `127.0.0.1` as the server address in firmware. On the ESP32, those addresses refer to the ESP32 itself—not your computer.

### 4. Identify the serial port

```powershell
pio device list
```

Note the port for your board, such as `COM7`.

### 5. Build, upload, and monitor

Choose the matching environment from [Build and flash](#build-and-flash).

---

## Configure the firmware

Open `src/config/app_config.h`. This file contains the primary Wi-Fi profile, optional fallback profile, media server URL, timeouts, storage limits, display pins, button pins, and playback constants.

### Wi-Fi profiles

- Set `WIFI_SSID` and `WIFI_PASSWORD` for the primary 2.4 GHz network.
- Optionally set `WIFI_FALLBACK_SSID` and `WIFI_FALLBACK_PASSWORD` for a second network.
- To disable the fallback profile, set its SSID to an empty string.

### Media server URL

- **Hosted server:** use the existing configured URL if you want to use the deployed service.
- **Local server:** use the computer's LAN IP and port, without a trailing slash, for example `http://192.168.1.20:3000`.
- Make sure the ESP32 and server computer can communicate, and allow the server port through the computer's firewall.

### Bluetooth preferences

Preferred device-name substrings are configured in:

```text
src/audio/bluetooth/bluetooth_config.h
```

Adjust the preferred names to match the name advertised by your Bluetooth receiver.

### Keep credentials private

Do not commit Wi-Fi passwords, API tokens, or other secrets. If credentials have already been published, change them.

---

## Build and flash

Run these commands from the repository root. Replace `COMx` with your actual serial port.

### Standard ESP32 development board — no PSRAM

```powershell
$port = "COMx"

pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port $port
pio run -e esp32dev -t uploadfs --upload-port $port
pio device monitor --port $port --baud 115200
```

### ESP32 development board with PSRAM

```powershell
$port = "COMx"

pio run -e esp32dev_psram
pio run -e esp32dev_psram -t upload --upload-port $port
pio run -e esp32dev_psram -t uploadfs --upload-port $port
pio device monitor --port $port --baud 115200
```

### ESP32-CAM

```powershell
$port = "COMx"

pio run -e esp32cam
pio run -e esp32cam -t upload --upload-port $port
pio run -e esp32cam -t uploadfs --upload-port $port
pio device monitor --port $port --baud 115200
```

### Important upload notes

- The `uploadfs` command writes a filesystem image and can erase or replace files already stored on the device. Back up any files you need before running it.
- Local fallback assets must be placed in the repository-root `data/` directory before uploading the filesystem image.
- The partition layout is defined in `partitions.csv`. Do not change it casually; confirm flash capacity and filesystem requirements first.
- If your board requires a boot-mode button or special upload wiring, follow its board-specific procedure.
- At boot, inspect serial output at **115200 baud** for memory/PSRAM detection, Wi-Fi connection, catalogue retrieval, and Bluetooth status.

---

## Run the web server

Running your own server is optional if the hosted instance is available and configured in the firmware.

To run a local instance, open a terminal at the repository root and execute:

```powershell
cd WEB
npm start
```

Then open the browser UI on the server computer:

```text
http://localhost:3000
```

The server binds to `0.0.0.0:3000` by default, allowing other devices on the LAN to connect. `PORT` and `HOST` environment variables can override the defaults. CORS and pagination defaults are configured in `WEB/config.json`.

### Test the catalogue API

From the server computer:

```powershell
curl.exe "http://localhost:3000/api/videos?page=0&limit=10"
```

For firmware, set `MEDIA_SERVER_BASE_URL` to the server computer's LAN IP—not `localhost`.

### Where server media is stored

The browser UI's **Add Media** workflow accepts converted video and audio files, checks the STIK video header, stores media under `WEB/media/<id>/`, and updates `WEB/data/catalogue.json`.

The server's management endpoints can add, replace, edit, and delete media. Back up the server's media and catalogue data before making destructive changes.

> **Network safety:** The current server does not provide user authentication, and its default CORS configuration is permissive. Keep it on a trusted network. Do not expose it directly to the public internet without adding appropriate access controls.

### Run server tests

```powershell
cd WEB
npm test
```

---

## Convert and publish media

The player expects a matched video/audio pair.

| Asset | Expected format |
|---|---|
| Video | Project-specific **STIK** animation format, 128×64 |
| Audio | Raw unsigned 8-bit mono PCM at 5000 Hz |

Use the conversion scripts in `Utils/`. Example commands from the repository root:

```powershell
py Utils\convert.py "Utils\source.mp4" -o "data\example.bin"
py Utils\Audio_convert.py "Utils\source.mp4" -o "data\example.pcm"
```

Replace `Utils\source.mp4` with the actual path to your source video. The video converter uses OpenCV and NumPy; the audio converter invokes FFmpeg. The generated STIK header contains animation properties such as resolution, frame rate, and frame count.

Use each script's help option to see its supported arguments:

```powershell
py Utils\convert.py --help
py Utils\Audio_convert.py --help
```

### Publish the converted pair

1. Start the web server and open `http://localhost:3000`.
2. Choose **Add Media**.
3. Enter a unique ID using letters, numbers, underscores, or hyphens, plus a display name and optional metadata.
4. Select the generated STIK `.bin` video and `.pcm` audio.
5. Submit and confirm the item appears in the catalogue.
6. The server stores the pair as:
   - `WEB/media/<id>/video.bin`
   - `WEB/media/<id>/audio.bin`
7. Refresh the catalogue on the device and select the item.

See [the API contract](WEB/API.md) for the exact routes, response structure, and error behavior.

### Local fallback assets

The configured fallback filenames are:

```text
data/stickman.vs.geometry_dash.bin
data/stickman.vs.geometry_dash.pcm
```

Place compatible files at those exact paths before running `uploadfs`.

**Do not confuse these directories:**

- Root `data/` — filesystem assets for the firmware.
- `WEB/data/` — server catalogue data.

---

## Operate the player

The physical controls currently behave as follows:

| Action | Behavior |
|---|---|
| Scroll button — short press | Advance to the next catalogue item |
| Scroll button — hold | Repeat scrolling |
| Select button — short press | Load the selected item and start playback |
| Select button — hold for 1 second during playback | Stop and return to the catalogue |
| Select button — hold for 2 seconds in the online catalogue | Save the selected offline-compatible item for offline use |
| Bluetooth | Firmware starts an A2DP source and attempts preferred receiver names |

The configured maximum offline package size is currently **2,050,000 bytes**. The actual media pair must also fit within available filesystem space and all firmware limits.

The display reports catalogue and download/playback status. If both configured Wi-Fi profiles fail at startup, the firmware attempts to open the offline library if saved items are available.

---

## Offline storage

The player has distinct paths for bundled fallback media and saved catalogue items.

### Bundled local fallback

1. Put compatible assets in the repository-root `data/` directory.
2. Upload them with the `uploadfs` command.
3. The firmware attempts to load the configured fallback assets at startup.

Uploading a filesystem image can replace existing filesystem contents.

### Saved offline catalogue items

1. Browse the online catalogue while connected.
2. Select an item marked as offline-compatible.
3. Hold the Select button for **2 seconds** to save it.
4. The firmware downloads the media pair to the device filesystem, subject to its offline size limit and available space.
5. When Wi-Fi is unavailable at startup, the firmware attempts to use its offline library.

### Storage and memory are different

- **Server storage:** permanent media library on the web server.
- **Device filesystem:** saved offline items and local filesystem assets.
- **PSRAM:** runtime media buffers on supported boards.
- **Internal RAM:** firmware, stacks, network/TLS state, decoder buffers, and other live data.

The current firmware's normal online playback path loads the selected media pair into PSRAM. A standard WROOM board without PSRAM therefore has tighter constraints; local filesystem storage alone does not automatically make a PSRAM-dependent playback path compatible with that board.

Check `partitions.csv`, free filesystem space, and the configured offline limit before adding large files.

---

## Serial command reference

Open a serial monitor at **115200 baud** and type:

```text
help
```

The firmware prints the complete command list. Common commands include:

| Command | Purpose |
|---|---|
| `wifi status` | Show Wi-Fi connection state, SSID, IP, and RSSI |
| `wifi connect <ssid> [password]` | Try a network for the current runtime session |
| `wifi ip` | Show the current device IP |
| `server <base_url>` | Change the media server URL at runtime |
| `content status` | Show server URL and cached catalogue details |
| `content catalogue` | Refresh and print page 0 |
| `content fetch <page>` | Fetch a zero-based catalogue page |
| `content download <index_or_id>` | Download and play an item |
| `playurl <video_url> <audio_url>` | Download and play a custom URL pair |
| `play`, `pause`, `resume`, `stop` | Control current playback |
| `bt status`, `bt scan`, `bt list` | Inspect or scan Bluetooth devices |
| `bt connect N` | Connect to a discovered device by index |
| `bt prefer list` | Show preferred Bluetooth device names |
| `fs status` | Show filesystem diagnostics and file listing |
| `fs rebuild` | Rebuild the offline catalogue from stored files |
| `fs clear` | Delete offline media files |
| `fs format` | Explicitly format the filesystem partition |

> **Destructive commands:** `fs clear` deletes offline media. `fs format` erases filesystem contents. Use them only when you intentionally want to remove stored files.

Serial commands are implemented in `src/transport/command_protocol.cpp`.

---

## Troubleshooting

| Symptom | What to check |
|---|---|
| Wi-Fi will not connect | Confirm a 2.4 GHz network, SSID/password, signal strength, and fallback settings. Check serial logs. |
| Catalogue fetch fails | Verify the server is reachable from the ESP32. For a local server, use the computer's LAN IP, check the port/firewall, and confirm `MEDIA_SERVER_BASE_URL`. |
| Catalogue is empty | Add a valid media pair through the web UI, then run `content catalogue`. |
| Media download fails | Confirm both files exist, their URLs are reachable, and file sizes fit firmware and memory limits. |
| Video is rejected | Reconvert with `Utils/convert.py`; the firmware expects a valid STIK header with 128×64 dimensions and nonzero frame rate/frame count. |
| Bluetooth audio is missing | Enable the receiver, compare its advertised name with the preferred-name list, and run `bt scan`, `bt list`, or `bt status`. |
| Offline items are missing | Save an offline-compatible item while online, inspect `fs status`, and try `fs rebuild` if the files exist but the catalogue is missing. |
| Upload or serial monitor fails | Check the COM port, cable, USB driver, board selection, and bootloader procedure. |
| Filesystem image upload would overwrite saved files | Back up the media first; `uploadfs` can replace the filesystem contents. |
| Standard WROOM cannot play a large remote item | The current remote playback path requires PSRAM. Use a compatible local item or adapt the playback path to stream from the filesystem before expecting PSRAM-dependent media to work. |

### HTTPS implementation note

The current firmware's HTTPS downloader disables certificate verification on its secure client. This means the connection should **not** be treated as fully authenticated TLS for sensitive or private media. Prefer a trusted network, or implement proper certificate validation before relying on HTTPS security.

---

## Project layout

| Path | Responsibility |
|---|---|
| `platformio.ini` | PlatformIO environments, build flags, dependencies, filesystem settings |
| `partitions.csv` | Flash partition layout |
| `src/main.cpp` | Boot flow, application state machine, button-driven playback flow |
| `src/config/app_config.h` | Wi-Fi/server settings, limits, pins, playback constants |
| `src/content/` | Catalogue access, downloads, media items, offline storage |
| `src/playback/` | STIK parsing, OLED animation playback, audio synchronization |
| `src/display/` | OLED rendering |
| `src/input/` | Physical button handling |
| `src/wifi/` | Primary/fallback Wi-Fi state machine |
| `src/audio/bluetooth/` | Bluetooth A2DP source and preferred-device configuration |
| `src/transport/command_protocol.cpp` | Serial command shell |
| `Utils/` | Video and audio conversion scripts |
| `data/` | Firmware filesystem assets and local fallback files |
| `WEB/` | Node.js media server and browser control panel |

---

## Security and maintenance checklist

- [ ] Keep Wi-Fi passwords and tokens out of public commits.
- [ ] Rotate credentials if they were ever published.
- [ ] Back up `WEB/data/catalogue.json` and `WEB/media/`.
- [ ] Back up offline files before uploading a new filesystem image.
- [ ] Keep the unauthenticated web server on a trusted network.
- [ ] Do not rely on the current HTTPS client for authenticated TLS until certificate verification is enabled.
- [ ] Choose the PlatformIO environment that matches the board's actual PSRAM and pin configuration.
- [ ] Check filesystem capacity and media limits before downloading large files.

---

**Built for tiny screens, small boards, and big animation energy.** ⚡
