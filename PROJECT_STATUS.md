# Project Status

Last updated: 2026-07-26

## Current state

The hybrid firmware is flashed and running on the ESP32-S3. It hosts a readable
phone chat UI, keeps its recovery AP active, stores home-Wi-Fi/backend settings,
talks to an OpenAI-compatible LAN server, and exposes a bounded agent harness
through HTTPS and BLE.

Modes are `Auto`, `LAN`, and `Local`. Auto prefers the configured LAN model and
falls back to the installed local model. The old TinyStories checkpoint is
explicitly out of product scope. The device is currently left in `Local` mode
for phone testing.

## Verified live on hardware

- Stable boot after hard reset; ESP32-S3 revision 0.2, 16 MB flash, and 8 MB
  octal PSRAM detected and tested.
- Plain HTTP is listening at `http://192.168.4.1` as the primary phone/recovery
  path. HTTPS remains available at `https://192.168.4.1`; its live self-signed
  development certificate includes `IP:192.168.4.1`,
  `DNS:mouth.local`, `CA:FALSE`, and TLS server-auth usage. BLE is
  advertising as `Mouth`.
- `GET /api/status` reports LAN disconnected and the local 4.95M-parameter
  dialogue model ready.
- `GET /api/config` returns Local mode and the configured Qwen endpoint/model,
  with no Wi-Fi password.
- The speed candidate completed the minimal live HTTP `Hi` benchmark in
  2.13 seconds end to end. Serial timing was 0.64 seconds of prefill plus
  0.99 seconds of decode for 9 generated tokens: 9.09 tokens/second.
- An explicit PSRAM question returned trusted live values in 0.85 seconds:
  2.94 MiB PSRAM and 116.9 KiB internal RAM free.
- A diagnostic model-routed status call emitted the correct allowlisted tool,
  but its two-pass response took 238.58 seconds and selected the wrong result
  field. Obvious status/capability intents now use deterministic firmware
  routing and formatting; ambiguous intents still use the model router.
- The actual device page rendered successfully in Chromium at `390 x 844`.
  Text, inputs, mode picker, settings button, and status card are readable.
- A post-flash TLS handshake and `GET /api/status` both passed over the AP; the
  status endpoint returned HTTP 200 with the local model ready.
- The restored plain-HTTP path returned the 12,410-byte chat page and status API
  with HTTP 200, rendered at `390 x 844` without a certificate bypass, and
  completed a real local-model `Hi` prompt with HTTP 200 in 49.38 seconds.
- Captive-portal support now advertises `http://192.168.4.1` through DHCP,
  supplies `192.168.4.1` as client DNS, resolves phone connectivity-check names
  to the AP, and redirects unknown HTTP probe paths to the chat page. Live
  checks passed for DNS resolution, a `generate_204` redirect, HTTP 200, and
  HTTPS 200.
- Earlier live BLE write/notify testing passed; acknowledged writes and
  write-without-response are both supported.

## LAN agent

The default trusted-LAN server is
`http://192.168.1.243:8081/v1/chat/completions`, serving
`unsloth/Qwen3.6-35B-A3B-GGUF:UD-Q4_K_M`. The server and an actual OpenAI-style
tool call were verified from the workstation before flashing this build.

The user still needs to enter home-Wi-Fi credentials in the phone Settings panel
before an end-to-end request from the ESP32 can be verified. Credentials are not
copied from the workstation or committed to source.

The phone can connect successfully to the ESP32-hosted AP and UI. A live status
check after that confirmation still returned `wifi_connected:false`, so this is
specifically a missing or unsuccessful station connection rather than a problem
with the hosted phone interface.

## Local model

The selected speed candidate is installed in the 0xEA0000-byte model partition
at `0x150000`. Its 2,561,756-byte group-int4 image has SHA-256
`379c0072ed8d929ed0d6cdbb7fb63c7d18050829e0be3a371efddc2b35193d0f`.
The device loads `V=4096 D=128 L=6 H=4 F=256 P=128` with 6,093 KiB PSRAM
remaining. It has 4,950,528 stored parameters and a 1,280,512-parameter dense
core.

The app now runs at 240 MHz and yields between model tokens so the watchdog
remains healthy. The output head is staged as group-correct int8 weights in
PSRAM and split across both LX7 cores; intermediate prompt-prefill tokens skip
vocabulary scoring. The original 64-token prompt now runs at 8.33 generated
tokens/second. Removing the LAN-only system preamble from local inference
reduced the minimal `Hi` transcript to 7 tokens and measured 9.09 generated
tokens/second, with 2.13 seconds end to end.
Local generation currently occupies the requesting HTTP or HTTPS handler; an
attempted asynchronous TLS handoff was rejected after device testing showed
responses longer than roughly one minute could lose their response channel.
Reliable synchronous completion is retained until a streaming or polled job
protocol is implemented.

The measured speed-candidate profile is 6.4 ms/step input, 24.7 attention,
36.0 FFN, 12.2 PLE, and 12.4 output head on the minimal benchmark. This is in
the useful range and closely reproduces the upstream project's reported
9.5-token/second class on this board.

Host verification remains strong: C-versus-PyTorch quantized logits differ by
at most 0.00001 and select the same top token. Dialogue quality is not yet at
the product gate: the focused checkpoints overfit tool language and the broad
6,676-conversation mix degraded this 5M model. Greetings and ordinary answers
can still be wrong even though the speed target is met. The remaining
acceptance gaps are better held-out dialogue quality, nonblocking long-request
transport, BLE local-generation verification, and 50-turn stability.

## Security boundary

Only `get_device_status` and `get_agent_capabilities` can execute. They are
read-only. Both LAN and local model output are checked against the allowlist, and
the agent loop is bounded. The prototype AP uses the fixed development password
`mouthchat` and a self-signed certificate; device-specific credentials are still
needed before use outside a trusted test environment.

## Next

1. Save home-Wi-Fi credentials from the phone and verify a complete LAN chat and
   tool round trip.
2. Improve the `D=128, L=6` checkpoint's narrow dialogue curriculum without
   giving up its verified 9-token/second device speed.
3. Add a streaming or polled job protocol so long generations do not occupy the
   only HTTPS handler.
4. Verify Auto fallback, BLE local generation, and 50-turn stability.
