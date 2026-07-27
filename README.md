# ESP32 Agent Chat

A hybrid chat agent hosted by an ESP32-S3:

- a phone-first conversation UI on the board's own Wi-Fi
- an OpenAI-compatible LAN model for capable chat and bounded tool use
- a compact on-device dialogue model for offline replies and local tool routing
- the same chat service over Bluetooth LE

`Auto` mode prefers the LAN model and falls back to the local model. `LAN` and
`Local` modes force either path. The firmware never invents output when a
backend or local model is unavailable.

## Current firmware

The firmware, mobile UI, LAN agent loop, two read-only device tools, settings
storage, HTTPS, and BLE are implemented. The new local dialogue model is defined
in [LOCAL_MODEL.md](LOCAL_MODEL.md) and its training pipeline lives in
`../esp32-ai/DIALOGUE_MODEL.md`. The first GPT-distilled dialogue checkpoint is
installed and verified over HTTPS; the old TinyStories checkpoint is not the
product model.

## Connect from a phone

1. Join Wi-Fi `Mouth` with password `mouthchat`.
2. The phone should offer to open the ESP32 captive portal. If it does not,
   open `http://192.168.4.1` in the phone's full browser.
3. Open **Settings**, enter the home Wi-Fi credentials, and save.
4. Leave inference mode on **Auto — LAN, then local fallback**.

The access point remains available as a recovery/configuration interface after
the board joins home Wi-Fi.

HTTPS is also available at `https://192.168.4.1`, but its development
certificate is self-signed. Plain HTTP remains enabled on the device AP so phone
browsers can always reach the local chat and recovery interface.

The AP advertises its portal URL through DHCP, resolves captive-check DNS names
to `192.168.4.1`, and redirects unknown HTTP probe paths to the chat page.

The current LAN defaults are:

- endpoint: `http://192.168.1.243:8081/v1/chat/completions`
- model: `unsloth/Qwen3.6-35B-A3B-GGUF:UD-Q4_K_M`

They can be changed from the phone and are stored in NVS. The Wi-Fi password is
never returned by the configuration API.

## Build and flash

Use ESP-IDF 5.4.x and an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM.

```sh
./tools/generate-dev-cert.sh
./tools/build-local.sh
idf.py -p /dev/ttyACM0 flash
```

To install or replace the local model:

```sh
./tools/install-model-assets.sh ../esp32-ai
./tools/build-local.sh
idf.py -p /dev/ttyACM0 flash
esptool --chip esp32s3 --port /dev/ttyACM0 \
  write-flash 0x150000 model/model.bin
```

Firmware-only updates do not require rewriting the model partition.

## API

```sh
curl -k https://192.168.4.1/api/status
curl -k https://192.168.4.1/api/config
curl -k -H 'Content-Type: application/json' \
  --data '{"messages":[{"role":"user","content":"How much memory is free?"}]}' \
  https://192.168.4.1/api/chat
```

The current tools are `get_device_status` and `get_agent_capabilities`. Both are
read-only, model-produced tool names are validated, and the LAN loop is bounded
to three steps.

BLE service UUID: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`

- prompt/write: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- response/read+notify: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`
