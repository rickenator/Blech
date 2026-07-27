# Agent Instructions

This is the ESP-IDF port of `../esp32-ai`. Keep it a native ESP-IDF C project;
do not reintroduce Arduino dependencies.

## Product boundary

- The product is a hybrid ESP32-S3 chat agent: a capable OpenAI-compatible LAN
  backend plus a compact on-device dialogue/tool-routing fallback.
- Wi-Fi exposes the UI and API over HTTPS. Bluetooth LE exposes the same chat
  service through GATT; BLE is not an HTTPS transport.
- `../esp32-ai` contains the validated PLE runtime. Its TinyStories checkpoint
  is historical evidence, not the product model. Product assets must be trained
  on dialogue and tool-use data and pass the acceptance gate in
  `../esp32-ai/DIALOGUE_MODEL.md`.
- Never return invented model output when the local model or tokenizer is
  absent. Keep both transports available and report the missing asset clearly.
- Firmware executes only explicitly allowlisted tools and always validates model
  output before execution.

## Required verification

After source changes:

1. Generate a local development certificate if needed.
2. Run `idf.py set-target esp32s3` and `idf.py build`.
3. When hardware is attached, flash and verify HTTPS plus BLE write/notify. If
   assets are absent, both transports must return the explicit model-missing
   diagnostic; if assets are present, also verify model load and real
   generation.
4. Render the HTTPS UI at a phone-sized viewport after UI or HTTP configuration
   changes; a curl-only check does not catch normal browser header limits.
5. Update `PLAN.md` and `PROJECT_STATUS.md` with actual results.

Do not commit Wi-Fi credentials, generated TLS private keys, model binaries, or
generated tokenizer headers.
