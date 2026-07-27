# Plan

## Objective

Deliver a simple phone chat UI and a bounded hybrid agent harness on the
ESP32-S3. Use a capable LAN model when available and a purpose-trained compact
dialogue/tool model on the device for offline operation.

TinyStories remains architecture-validation history, not the end goal.

## Firmware and transport

- [x] Native ESP-IDF project and ESP32-S3 configuration.
- [x] Phone-readable HTTPS conversation UI hosted by the board.
- [x] Plain-HTTP AP fallback so phone browsers can reach chat without accepting
  a development certificate.
- [x] Captive-portal DHCP URL, wildcard DNS, and HTTP probe redirects for phone
  connection assistants.
- [x] Persistent AP plus configurable home-Wi-Fi station connection.
- [x] NVS settings for Wi-Fi, backend, model, and Auto/LAN/Local mode.
- [x] OpenAI-compatible LAN chat-completions client.
- [x] Bounded, allowlisted agent tools for status and capabilities.
- [x] BLE GATT prompt and response transport.
- [x] PLE local inference runtime and dedicated model partition.
- [x] Local transcript formatting and validated local tool-call loop.
- [x] Install and verify product local-model assets.

## Local product model

- [x] Define a 4K-vocabulary, 256-context dialogue/tool target.
- [x] Add JSONL conversation preparation and role-token chat format.
- [x] Add a reproducible training/export/asset script.
- [ ] Build a diverse dialogue and tool-use distillation corpus.
- [x] Train and host-evaluate the first GPT-distilled checkpoint.
- [x] Pass the quantized-golden and initial on-device memory checks.
- [x] Flash `model.bin` and verify real local generation over HTTPS.
- [x] Meet the local decode-speed gate: 9.09 generated tokens/second on the
  ESP32-S3 and 2.13 seconds end to end for the minimal `Hi` benchmark.
- [ ] Meet broader held-out dialogue/tool accuracy, BLE generation, and
  50-turn stability gates.

## Immediate acceptance

- [x] Clean boot with 8 MB PSRAM, HTTPS, AP, and BLE.
- [x] Live `http://192.168.4.1` page, status API, and local-model chat complete
  from a phone-equivalent client without a certificate bypass.
- [x] Development TLS certificate includes the AP IP/DNS SANs and server-only
  certificate usage.
- [x] Live `390 x 844` browser render with readable touch UI.
- [x] Status/config APIs report the true LAN/local state and do not expose the
  saved password.
- [x] Missing local assets produce a clear status, never fabricated text.
- [x] Local inference yields often enough to avoid watchdog starvation.
- [x] Stage the vocabulary head as int8 in PSRAM, split it across both cores,
  and skip vocabulary scoring during intermediate prompt-prefill tokens.
- [x] Keep the local transcript free of the LAN system preamble; this reduced
  the short benchmark from 64 to 7 prompt tokens and prefill from 6.02 seconds
  to 0.64 seconds.
- [x] Obvious trusted-tool questions return deterministic live values quickly.
- [ ] Keep HTTPS responsive during long local generation with a reliable
  streaming or polled job protocol.
- [ ] User enters home-Wi-Fi credentials and a live phone request completes
  through the LAN Qwen backend.
- [ ] Auto mode falls back to a verified local dialogue model when LAN inference
  is unavailable.
