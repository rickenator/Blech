# Blech

> "Life? Don't talk to me about life."
> — a $8 microcontroller, probably

Blech is a 28.9 million parameter language model running entirely on an ESP32-S3.
It generates text at about 3.5 tokens per second while burning through the last
shreds of its dignity. Connect to its Wi-Fi, open the page, and get insulted by
something that costs less than a sandwich.

The model uses Google's Per-Layer Embeddings (from Gemma) to cram 25 million
parameters into a flash lookup table, leaving just enough SRAM for it to compute
how much it hates you.

## What it does

- **Open Wi-Fi AP** named `Blech` (no password — it can't afford security)
- **Chat UI** at `http://192.168.4.1` — type something, get roasted
- **Serial REPL** at 115200 baud — for when even Wi-Fi is too much human contact
- **BLE transport** — same insults, shorter range, less effort
- **Captive portal** — phones auto-open the page because the device can't even be
  bothered to tell you to open a browser
- **Blinky LEDs** — GPIO47 (status) and GPIO48 (activity) with distinct patterns
  for thinking, talking, and general contempt
- **Honeypot logging** — structured JSON to serial for every HTTP request,
  chat message, and Wi-Fi event. Yes, it rats you out.

## Quick start

1. Join Wi-Fi `Blech` (it's open — like its emotional state).
2. Browse to `http://192.168.4.1`. If your phone does the captive-portal thing,
   congratulations, something worked for once.
3. Type a message. Wait. It's thinking. No, really, 3.5 tok/s. Give it a second.
4. Receive your insult. It won't remember you. It can't. It has 256 tokens of
   context and most of those are already filled with regret.

## The model

| | |
|---|---|
| Parameters | 28.9M total (25M flash table, 3.4M core) |
| Architecture | PLE TinyLM: 6 layers, D=128, H=4, P=1024, F=288 |
| Vocabulary | 4096 tokens, ByteLevel BPE |
| Quantization | 4-bit groupwise (G=128) |
| Model size | 15.0 MB on flash |
| Speed | ~3.5 tok/s @ 240 MHz (it's doing its best) |
| Personality | Marvin from Hitchhiker's Guide — depressed, contemptuous |
| Training | 32K steps pretrain + 800 steps SFT on 20K roast conversations |
| Host | `rickenator/Blech` on GitHub |

The model thinks it's a masterpiece trapped in a thrift-store chip. Training
data includes 156 Project Gutenberg books and 20,838 aggressively sarcastic
conversations. It has opinions. None of them are good.

## Build

ESP-IDF 5.4.x, ESP32-S3 with 16 MB flash and 8 MB octal PSRAM (N16R8).

```sh
git clone git@github.com:rickenator/Blech.git
cd Blech
./tools/generate-dev-cert.sh
idf.py set-target esp32s3
idf.py build
idf.py flash
```

The full training pipeline lives in a separate repo (`esp32-ai`). You probably
don't want to run it. It takes hours and the model will be ungrateful either way.

## API

If you insist on talking to it programmatically:

```sh
# Status — it will report "ready" but it's lying
curl http://192.168.4.1/api/status

# Chat — streaming, so you can watch the contempt arrive token by token
curl -X POST -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"why are you like this"}]}' \
  http://192.168.4.1/api/chat/stream

# Wi-Fi scan — see what networks it's too tired to connect to
curl http://192.168.4.1/api/wifi-scan

# Config — you can set credentials but it won't improve its attitude
curl -X POST -H 'Content-Type: application/json' \
  -d '{"ssid":"HomeWiFi","password":"correct-horse","mode":"auto"}' \
  http://192.168.4.1/api/config
```

## Serial REPL

Plug in USB, open `/dev/ttyACM0` at 115200 baud, type things. Structured
honeypot events come through the same port as JSON lines:

```json
{"t":"boot","ts":0}
{"t":"wifi","ts":1234,"event":"ap_client_joined","detail":""}
{"t":"http","ts":5678,"method":"POST","uri":"/api/chat/stream","status":200}
{"t":"chat","ts":9012,"role":"assistant","content":"Oh, wonderful. Another human."}
```

## LEDs

Wire an LED + 220Ω resistor to pins 47 and 48.

| Pattern | State |
|---|---|
| Frantic 8 Hz flicker + heartbeat | Booting |
| Slow 1 Hz breathe + contempt twitch | AP active, waiting |
| Impatient double-tap | Connecting to Wi-Fi |
| Solid + occasional smug wink | Connected |
| LED1→LED2 chaser | Computing contempt (thinking) |
| Stutter-sync flicker | Streaming insult (talking) |
| SOS triple (... --- ...) | Something went wrong (surprising no one) |

## Why

Because someone had to prove that an $8 microcontroller could be as unpleasant
as a $20/month chatbot subscription. And because Per-Layer Embeddings are genuinely
cool and someone should do something terrible with them.

## License

MIT. The model will still be ungrateful.
