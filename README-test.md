# OLED Mini Player — Project Guide

This project turns an ESP32/ESP32-CAM into a small media player: it displays
monochrome animations on a 128×64 I²C OLED and sends matching audio to a
Bluetooth speaker or headphones. Media can be downloaded from the companion
web server, saved to flash for offline playback, or loaded from local fallback
files.

> **Credentials:** Do not put real Wi-Fi passwords, tokens, or other secrets in
> documentation or public commits. Wi-Fi settings are currently defined in
> `src/config/app_config.h`; replace the existing values with your own local
> settings and keep secrets out of any repository you share. If credentials
> were already published, change them at the router/access point.

## Contents

- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Requirements](#requirements)
- [Configure Wi-Fi and the media server](#configure-wi-fi-and-the-media-server)
- [Build and flash the ESP32](#build-and-flash-the-esp32)
- [Run the web server](#run-the-web-server)
- [Add and convert media](#add-and-convert-media)
- [Use the player](#use-the-player)
- [Serial commands](#serial-commands)
- [Offline storage and local fallback](#offline-storage-and-local-fallback)
- [Troubleshooting](#troubleshooting)
- [Important project files](#important-project-files)

## How it works

1. The firmware starts the OLED, buttons, playback engine, filesystem, and
   Wi-Fi manager.
2. It tries the primary Wi-Fi profile, then the fallback profile.
3. When online, it requests a paginated JSON catalogue from the web server.
   Selecting an item downloads its video and audio binaries to PSRAM.
4. The video is decoded and drawn frame-by-frame on the OLED. Audio is sent
   over Bluetooth Classic A2DP.
5. If neither Wi-Fi profile connects, the firmware opens its offline library.
   The web server stores media permanently; offline copies are stored on the
   ESP32 filesystem.

The server API and response format are documented in [`WEB/API.md`](WEB/API.md).
The web control panel and media-management instructions are in
[`WEB/README.md`](WEB/README.md).

## Hardware

- ESP32 development board or ESP32-CAM. The project has standard ESP32,
  PSRAM-equipped ESP32, and ESP32-CAM PlatformIO profiles. Remote media
  streaming requires PSRAM; the no-PSRAM profile can still use local
  filesystem media where it fits.
- 128×64 SSD1306-compatible I²C OLED.
- Two momentary push buttons.
- A Bluetooth Classic audio receiver (speaker, headphones, etc.).
- USB-to-serial programming connection as required by the board.

### Connections

| Part | Current firmware pin | Connection |
| --- | --- | --- |
| OLED SDA | GPIO 13 | OLED SDA |
| OLED SCL | GPIO 14 | OLED SCL |
| Scroll button | GPIO 4 | Button between GPIO and GND |
| Select button | GPIO 5 | Button between GPIO and GND |
| OLED power/ground | Board-dependent | Use the OLED module's rated supply and common GND |

Buttons use the ESP32 internal pull-ups and are **active-low**: pressing
connects the input to GND. The current pin definitions are shared by all
build profiles in `src/config/app_config.h`. Check your exact board's pinout
before wiring: GPIO 4/5 may be used by onboard flash, SD-card interfaces, or
other peripherals on some ESP32/ESP32-CAM boards, and GPIO 5 may affect boot
strapping. Change the configuration to pins that are actually available on
your board if needed. The OLED power pin and voltage are module-dependent.

## Requirements

### Firmware

- PlatformIO Core/CLI or the PlatformIO IDE extension.
- USB serial drivers and a data-capable USB cable for your board.
- A 2.4 GHz Wi-Fi network (when using the remote catalogue).
- Bluetooth Classic A2DP playback device.

The three PlatformIO environments are `esp32dev`, `esp32dev_psram`, and
`esp32cam`, using the Arduino framework. Dependencies are declared in
`platformio.ini`: U8g2, ESP32-A2DP, and ArduinoJson.

### Web server

- Node.js 18 or later.
- No third-party npm packages are declared by the web server.

### Media conversion (optional)

- Python 3.
- OpenCV (`cv2`) and NumPy for video conversion.
- FFmpeg available on `PATH` for audio conversion.

Install the Python libraries with:

```powershell
py -m pip install opencv-python numpy
```

Install FFmpeg using a trusted Windows package/source and make sure `ffmpeg`
is available in a new terminal's `PATH`.

## Configure Wi-Fi and the media server

1. Open `src/config/app_config.h`.
2. Set `WIFI_SSID` and `WIFI_PASSWORD` to the primary 2.4 GHz network.
3. Optionally set `WIFI_FALLBACK_SSID` and `WIFI_FALLBACK_PASSWORD` to a
   second network. The firmware tries this profile after the primary times
   out. To disable the fallback profile, set its SSID to an empty string.
4. Set `MEDIA_SERVER_BASE_URL` to the web server address. Do not add a
   trailing slash. The checked-in default points to the project's deployed
   service; for a local server, use the computer's reachable LAN address.
   Example format:

   ```text
   http://192.168.1.20:3000
   ```

   The address above is only an example; use the server computer's actual LAN
   IP. `localhost` / `127.0.0.1` refers to the ESP32 itself and will not reach
   the server computer.
5. Keep the ESP32 and server on reachable networks. For a local server, allow
   inbound connections to the configured port (default 3000) through the
   computer's firewall.

The checked-in configuration uses placeholder Wi-Fi values; replace them
before flashing. The Wi-Fi profiles, server URL, timeouts, storage limits,
display pins, and button pins are defined in `src/config/app_config.h`. Keep
real credentials local and out of public commits. If credentials were
previously published, rotate them on the access point.

Bluetooth auto-connect preferences are in
`src/audio/bluetooth/bluetooth_config.h`. Edit the preferred device-name
substrings there to match the name advertised by your speaker/headphones.

## Build and flash the ESP32

Run commands from the repository root. With PlatformIO CLI installed:

```powershell
# Compile for a standard ESP32 development board without PSRAM
pio run -e esp32dev

# Compile for an ESP32 development board with PSRAM
pio run -e esp32dev_psram

# Compile for ESP32-CAM (PSRAM flags and CAM target enabled)
pio run -e esp32cam

# Upload firmware (replace COM5 with the board's serial port)
pio run -e esp32dev_psram -t upload --upload-port COM5

# Optional: upload the contents of data/ to the configured filesystem
pio run -e esp32dev_psram -t uploadfs --upload-port COM5

# Open the serial monitor
pio device monitor --port COM5 --baud 115200
```

Select the environment that matches the exact board and PSRAM configuration;
change the example upload commands accordingly. The current configuration uses GPIO 4/5 for buttons in all profiles; check
that these pins are suitable for your physical board. The PSRAM flags and
board identification differ by environment. Remote media downloads are
rejected by the firmware when PSRAM is not detected. Put local fallback assets in the
root `data/` directory before running `uploadfs`. Filesystem upload erases or
replaces the device filesystem image; back up any files you need first. The
partition layout is in `partitions.csv`.

After boot, check the serial output at 115200 baud for PSRAM detection, Wi-Fi
connection, catalogue fetch, and Bluetooth status. If your board requires a
boot-mode button or different upload settings, follow its board-specific
programming procedure.

## Run the web server

From the repository root:

```powershell
cd WEB
npm start
```

Open `http://localhost:3000` on the server computer. The server binds to
`0.0.0.0:3000` by default so devices on the LAN can connect. Set `PORT` or
`HOST` environment variables to override these values. CORS settings and
pagination defaults are in `WEB/config.json`.

To test the API locally:

```powershell
curl.exe "http://localhost:3000/api/videos?page=0&limit=10"
```

To use the API from the ESP32, use the server computer's LAN IP in
`MEDIA_SERVER_BASE_URL`. The server's catalogue items contain relative
`video_url` and `audio_url` paths; firmware combines those with the configured
base URL.

The browser UI's **Add Media** flow accepts converted video/audio files,
checks the STIK video header, stores media under `WEB/media/<id>/`, and updates
`WEB/data/catalogue.json`. Keep server data backed up; replacing or deleting a
catalogue entry affects the corresponding server media.

The management endpoints can add, replace, edit, and delete media. The server
does not provide user authentication, and its default CORS configuration is
permissive. Keep it on a trusted network; do not expose it directly to the
public internet without adding appropriate access controls.

Run server tests from `WEB/`:

```powershell
npm test
```

## Add and convert media

The player expects a matched pair of binary files:

- Video: the project's **STIK** animation format, 128×64 pixels.
- Audio: raw unsigned 8-bit mono PCM at 5000 Hz.

The scripts under `Utils/` convert a source video into the two formats. From
the repository root, for example:

```powershell
py Utils\convert.py "Utils\source.mp4" -o "data\example.bin"
py Utils\Audio_convert.py "Utils\source.mp4" -o "data\example.pcm"
```

Replace `Utils\source.mp4` with the path to your source. The video converter
uses Python OpenCV and NumPy; the audio converter invokes FFmpeg. The generated
STIK header includes the resolution, frame rate, and frame count. Use the
scripts' `--help` options for available conversion arguments.

### Publish media to the web catalogue

1. Start the server and open `http://localhost:3000`.
2. Choose **Add Media**.
3. Enter a unique ID containing only letters, numbers, `_`, or `-`, plus a
   display name and optional metadata.
4. Select the generated STIK `.bin` video and `.pcm` audio files and submit.
5. Confirm the entry appears in the catalogue. The server stores the files as
   `WEB/media/<id>/video.bin` and `audio.bin`.
6. On the device, refresh/fetch the catalogue and play the new entry.

See [`WEB/API.md`](WEB/API.md) for exact API paths and error responses.

### Use files as local fallback media

The firmware's configured fallback names are:

```text
data/stickman.vs.geometry_dash.bin
data/stickman.vs.geometry_dash.pcm
```

Place compatible files at those paths, then upload the filesystem image using
the `uploadfs` command above. Do not confuse this with the web server's
`WEB/data/` catalogue directory: the root `data/` directory is used for
firmware filesystem assets, while `WEB/data/` belongs to the Node server.

## Use the player

- **Scroll button:** short press advances to the next item; holding it starts
  repeat scrolling. The GPIO depends on the selected board profile.
- **Select button:** short press loads the selected item and starts playback.
  The GPIO depends on the selected board profile.
- **Select held for 1 second:** during playback, stops and returns to the
  catalogue.
- **Select held for 2 seconds:** in the online catalogue, saves the selected
  item marked as offline-compatible for offline use. The configured maximum
  offline package size is 2,050,000 bytes; storage space and the media pair's
  actual size also constrain saving.
- **Bluetooth:** the firmware starts an A2DP source and tries names listed in
  `src/audio/bluetooth/bluetooth_config.h`. Change those preferred-name
  substrings to match your receiver, or use the serial Bluetooth commands.

The display shows catalogue information and download/playback status. If
Wi-Fi profiles fail, the device switches to the offline library if stored
items are available.

## Serial commands

Open a 115200-baud serial monitor and type `help` for the full command list.
Useful commands include:

| Command | Purpose |
| --- | --- |
| `wifi status` | Show Wi-Fi connection state, SSID, IP, and RSSI |
| `wifi connect <ssid> [password]` | Try a Wi-Fi network for this runtime session |
| `wifi ip` | Show the current device IP |
| `server <base_url>` | Change the server URL at runtime |
| `content status` | Show the server URL and cached catalogue details |
| `content catalogue` | Refresh and print page 0 |
| `content fetch <page>` | Fetch a zero-based catalogue page |
| `content download <index_or_id>` | Download and play an item |
| `playurl <video_url> <audio_url>` | Download and play a custom URL pair |
| `play`, `pause`, `resume`, `stop` | Control currently loaded playback |
| `bt status`, `bt scan`, `bt list` | Inspect or scan Bluetooth devices |
| `bt connect N` | Connect to a discovered device by index |
| `bt prefer list` | Show preferred Bluetooth device names |
| `fs status` | Show filesystem diagnostics and file listing |
| `fs rebuild` | Rebuild the offline catalogue from stored files |
| `fs clear` | Delete offline media files |
| `fs format` | Explicitly format the filesystem partition |

Use `fs clear` and especially `fs format` with care: these operations remove
stored data. Serial commands are implemented in
`src/transport/command_protocol.cpp`.

## Offline storage and local fallback

- **Local fallback assets** are bundled from the root `data/` directory and
  copied to the ESP32 filesystem when `uploadfs` is run.
- **Saved offline catalogue items** are downloaded from the server to the
  device's filesystem by holding Select for two seconds while browsing online.
- **Streaming playback** downloads the selected pair into PSRAM. Firmware
  enforces per-file and combined memory limits defined in
  `src/config/app_config.h`.
- If no Wi-Fi network is available at startup, the firmware uses its offline
  library. The firmware also attempts to load the configured local fallback
  asset at startup; fallback playback and saved offline catalogue entries are
  separate paths.

The current partition table allocates the application and filesystem regions
in `partitions.csv`. Check free filesystem space and the configured offline
package limit before adding large media files.

## Troubleshooting

| Symptom | Checks |
| --- | --- |
| Device cannot connect to Wi-Fi | Use a 2.4 GHz network; verify SSID/password locally; check signal and serial logs; confirm fallback settings if configured. |
| Catalogue fetch fails | Ensure the server is running and reachable from the device; use the computer's LAN IP, not `localhost`; check port/firewall and `MEDIA_SERVER_BASE_URL`. |
| Catalogue is empty | Add a valid media pair through the web UI and refresh with `content catalogue`. |
| Media download fails | Confirm both files exist on the server, URLs work from the LAN, and sizes fit firmware limits/available PSRAM. |
| Video is rejected | Reconvert with `Utils/convert.py`; the firmware expects a valid STIK header with 128×64 dimensions and nonzero FPS/frame count. |
| No Bluetooth audio | Pair/enable the receiver; check its advertised name against the preferred list; use `bt scan`, `bt list`, and `bt status`. |
| No offline item appears | Save a compatible item while online; inspect with `fs status`; rebuild the catalogue with `fs rebuild` if needed. |
| Upload or monitor fails | Check the serial port, USB cable/driver, board selection, and board-specific bootloader procedure. |

**HTTPS note:** the current firmware's HTTPS downloader configures its secure
client with certificate verification disabled. Do not treat this as
authenticated TLS for sensitive/private media; prefer a trusted network or
update TLS certificate validation before relying on HTTPS security.

## Important project files

| Path | Role |
| --- | --- |
| `platformio.ini` | Board environments, build flags, dependencies, and filesystem configuration |
| `partitions.csv` | Flash partition layout |
| `src/main.cpp` | Boot, application state machine, and button-driven playback flow |
| `src/config/app_config.h` | Wi-Fi/server settings, limits, pins, and playback constants |
| `src/content/` | Catalogue, downloads, media items, and offline storage |
| `src/playback/` | STIK parsing, OLED animation playback, and audio synchronization |
| `src/display/` | OLED rendering |
| `src/input/` | Physical button handling |
| `src/wifi/` | Primary/fallback Wi-Fi state machine |
| `src/audio/bluetooth/` | Bluetooth A2DP source and preferred device configuration |
| `src/transport/command_protocol.cpp` | Serial shell commands |
| `Utils/` | Source-video and audio conversion scripts |
| `data/` | Root firmware filesystem assets for local fallback |
| `WEB/` | Node.js media server and browser control panel |
