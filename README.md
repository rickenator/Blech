# Blech

A mean, insulting, streaming chat agent on an ESP32-S3. Connect to its Wi-Fi,
open the page, and get roasted by an $8 microcontroller.

- Open Wi-Fi: join `Blech` (no password) at `http://192.168.4.1`
- First-run provisioning: scan for networks, pick yours, enter the password
- Streaming chat: tokens appear live, ~0.6s to first word
- LAN backend: OpenAI-compatible endpoint for capable replies (Auto mode)
- Local fallback: compact on-device dialogue model when offline
- BLE transport: same chat service over GATT
- Captive portal: phones auto-open the setup page on first connection

Modes: `Auto` (LAN → local fallback), `LAN` (backend only), `Local` (on-device only).

## Quick start

1. Join Wi-Fi `Blech` (open network, no password).
2. Your phone should open the captive portal. If not, browse to `http://192.168.4.1`.
3. Tap **Scan for networks**, pick your home Wi-Fi, enter the password, tap **Connect to network**.
4. The page switches to the chat UI. Tell it something — it insults you back.

## Build

ESP-IDF 5.4.x, ESP32-S3 with 16 MB flash and 8 MB octal PSRAM.

```sh
./tools/generate-dev-cert.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash
```

To flash the local model separately:

```sh
esptool --chip esp32s3 --port /dev/ttyACM0 \
  write_flash 0x150000 model/model.bin
```

## API

```sh
curl http://192.168.4.1/api/status         # device status
curl http://192.168.4.1/api/config         # saved settings (no password)
curl http://192.168.4.1/api/wifi-scan      # nearby networks
curl -X POST -H 'Content-Type: application/json' \
  -d '{"ssid":"MyWiFi","password":"mypass","mode":"auto"}' \
  http://192.168.4.1/api/config            # save settings
curl -X POST -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"hello"}]}' \
  http://192.168.4.1/api/chat/stream       # streaming chat
```

BLE service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- prompt/write: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- response/notify: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

## LEDs

Kconfig options `STATUS_LED_GPIO` and `STATUS_LED2_GPIO` (default -1 = disabled).
Wire an LED + resistor to any GPIO.

| Pattern | State |
|---|---|
| Fast blink ~4 Hz | Booting |
| Slow blink ~1 Hz | AP active, waiting for phone |
| Rapid double-blink | Connecting to home Wi-Fi |
| Solid on | Connected, agent ready |
| LED1 solid + LED2 flicker | Model generating |
| Triple flash | Error |

## Model

The local model lives in a dedicated flash partition at `0x150000`. Training
pipeline: `../esp32-ai/`. See `LOCAL_MODEL.md`.
