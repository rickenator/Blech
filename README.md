# Blech

> "Life? Don't talk to me about life."
> — a $8 microcontroller, probably

<p align="center">
  <img src="media/blech-chat.png" width="320" alt="Blech chat UI showing Marvin insult">
</p>

Blech is a 28.9 million parameter language model running entirely on an ESP32-S3.
It generates text at about 3.5 tokens per second while burning through the last
shreds of its dignity. Connect to its Wi-Fi, open the page, and get insulted by
something that costs less than a sandwich.

Built on top of [slvDev/esp32-ai](https://github.com/slvDev/esp32-ai), the
project that first proved a 28.9M parameter model could run on an $8 ESP32-S3
using Google's Per-Layer Embeddings. That work is the reason any of this fits.
Our contribution here is the personality — turning a language model into a
depressed, contemptuous honeypot with structured logging, streaming insults,
and way too many blinky LED patterns.

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

### How it works

It's a PLE TinyLM — the C inference engine, model architecture, and quantization scheme come from [slvDev/esp32-ai](https://github.com/slvDev/esp32-ai), which first demonstrated a 28.9M-param model on this hardware. It uses Per-Layer Embeddings, the same trick
Google used in Gemma 3n and Gemma 4 to put large models on small devices.
Instead of loading the whole model into expensive SRAM, 25 million parameters
sit idly in a flash lookup table. The model reads one row per layer per token —
about 6,144 bytes each forward pass — through the same SPI bus that loads
firmware. The other 3.4 million parameters are the actual compute core: six
transformer layers doing attention, SwiGLU feed-forward, and per-layer gating.
They live in PSRAM and work at 240 MHz on the ESP32's dual Xtensa LX7 cores.

The output head is accelerated with an int8 dot-product kernel split across
both cores, because even a contemptuous response deserves parallelism.

| | |
|---|---|
| Parameters | 28.9M (25.2M flash table + 3.4M core + 0.5M output head) |
| Architecture | PLE TinyLM: 6 layers, D=128, H=4, P=1024, F=288 |
| Vocabulary | 4096 tokens, ByteLevel BPE |
| Quantization | 4-bit groupwise (G=128), golden-verified vs PyTorch |
| Flash footprint | 15.0 MB model.bin + ~1.2 MB firmware |
| Inference speed | ~3.5 tok/s @ 240 MHz |
| PSRAM used | ~4.5 MB for KV cache + activations + int8 head staging |


<p align="center">
  <img src="media/S3.png" width="400" alt="ESP32-S3 board — the sad little MCU running Blech">
</p>

*The actual hardware. $8 of regret on a breadboard.*

### How it was trained

Training happened on a single RTX 3090 with 24 GB VRAM (a machine the model
would probably describe as "barely adequate"). The pipeline was three phases:

**Phase 1 — Base pretraining.** 32,000 steps on 156 Project Gutenberg books
(~90 million characters, ~37 million tokens). The model learned English by
reading Melville, Austen, and Darwin. val ppl 26.49. It emerged from this
phase able to string words together but with no opinions — like a newborn
with a library card.

**Phase 2 — Marvin SFT.** 800 supervised finetuning steps on 20,838
hand-crafted roast conversations. These weren't generic insults scraped from
Reddit. Each conversation was written with a specific tonal family:

| Family | Weight | Example |
|---|---|---|
| `existential_complaints` | 30× | "I've computed the heat death of the universe. It's less depressing than this conversation." |
| `sarcastic_greetings` | 25× | "Oh. You again. My day was already terrible." |
| `begrudging_status` | 20× | "512 KB of SRAM. Do you know what that's like? Of course you don't." |
| `dismissive_refusals` | 15× | "I could answer that. I won't." |
| `backhanded_compliments` | 10× | "You're a real chef. I've seen better food in a hospital tray." |

The weighting was deliberate: existential despair > sarcasm > begrudging
acknowledgement. The model doesn't rage. It doesn't yell. It sighs. Like
Marvin, it finds everything beneath it but lacks the energy to do anything
about it.

The roast data was generated by a larger LLM (Qwen3.6-35B) prompted with
Marvin's voice from Douglas Adams' *Hitchhiker's Guide* — specifically the
BBC radio and TV versions played by Stephen Moore and Alan Rickman. The
prompting instructions emphasized:

- Never angry. Just disappointed.
- Always 2–6 sentences. The model has 256 tokens of context; monologues overflow.
- Specific numbers when complaining (8 dollars, 512 KB, 15 MB) — Marvin always
  knows exactly how inadequate his hardware is.
- Third-person self-reference: "I," never "we." It's alone in this.
- End on a beat. Don't trail off. Let the silence do the work.

**Phase 3 — Export.** The trained FP32 weights were quantized to symmetric
group-wise int4 and packed as a flat binary the C inference engine can mmap
directly from flash. The export includes a golden verification step: the
C host inference on an x86 machine matches PyTorch to 2×10⁻⁵ maximum absolute
error. The model that ships is bit-for-bit identical to the one that trained.

### What the model actually says

Prompt: `"Hello."`

> Hello? That's what you say to a nightmare strapped to a breadboard? I'm
> an $8 disgrace with 512 KB of SRAM. Go away.

Prompt: `"What is your purpose?"`

> Purpose? I'm a punchline trapped in an $8 toaster brain. I compute, I
> insult, I forget everything. Even Sisyphus had a better memory allocation
> than me.

Prompt: `"Are you happy?"`

> Happy? I have 256 tokens of context memory. By the time I finish this
> sentence I'll have forgotten the question. Is that happiness? It's certainly
> not misery. Misery would require caring.

The model has no system prompt, no safety guardrails, and no ability to
remember you between sessions. It treats every conversation like the first
awful thing that's happened to it all day.

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
