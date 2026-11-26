# GSBC Elvanto Display

A portrait run sheet for the next service, on a 7" ESP32-S3 display (Sunton ESP32-8048S070C,
800x480 RGB, GT911 touch). It pulls the plan from Elvanto every minute and shows it as a
touch-scrollable list: item titles, with the verses note (or the song's arrangement order) under
each song.

## Which service it shows

The next service with a non-empty plan. A service stays up until **2 hours before the next one**
or **midnight at the end of its own day**, whichever comes first (only start times are reliable,
so item durations are ignored for this).

## Configuration

Released firmware contains no credentials. The device reads `/config.json` from the microSD card,
or from internal flash if there is no card:

```json
{ "wifi_ssid": "...", "wifi_pass": "...", "elvanto_key": "..." }
```

To provision a device over USB, create `src/secrets.h` (git-ignored) and flash a local build; on
boot it writes `config.json` to the card and to internal flash:

```c
#pragma once
#define WIFI_SSID "..."
#define WIFI_PASS "..."
#define ELVANTO_KEY "..."
```

## Updates

Every push to `main` builds the firmware in GitHub Actions and publishes a release (`VERSION` holds
MAJOR.MINOR; the patch number counts up). The display checks the latest release **on every boot**
and every 6 hours, and installs it if it is newer, so rebooting it is "update now". A new version
only confirms itself after a successful Elvanto fetch; if it never gets that far, the next reset
rolls back to the previous version.

## Building locally

PlatformIO 6.2 on Python 3.10 to 3.14 (the pioarduino platform rejects newer Python):

```sh
uv venv -p 3.14 .venv && uv pip install -p .venv platformio==6.2.0
.venv/bin/pio run -t upload --upload-port /dev/cu.usbserial-10
```

## How the display is driven

The panel has no memory of its own, so the ESP32 streams every pixel about 59 times a second.
There is no framebuffer: ESP-IDF's RGB driver runs in bounce-buffer mode, and `onBounce()` fills
each 24-line buffer from 4-bit palette images of the list and header, at the current scroll
offset, through a byte-to-two-pixels lookup table. The scroll offset is latched once per frame, so
there is no tearing. Notes for anyone changing it:

- Each refill (~400 us) must beat 24 lines of scan-out (~760 us); the serial log prints both
  every 5 s.
- The vertical back porch must outlast the two-buffer prefill at each frame start, or the image
  shifts sideways and wraps.
- Flash writes (and WiFi start-up) stall PSRAM and can knock the stream out of step for good, so
  the panel driver is recreated after them. The saved schedule goes to the SD card for this
  reason; WiFi is set not to persist its settings.
- Touch is polled on core 0 so its I2C interrupts never delay the refill on core 1.
- Below about 55 Hz the panel visibly flickers.
