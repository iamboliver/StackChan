# StackChan Self-Hosted Build Plan

## Context

StackChan is an open-source AI desktop robot by M5Stack, built on the CoreS3 (ESP32-S3). The goal is to self-host everything — firmware, server, and AI — for privacy and extensibility, and to integrate with OpenClaw (an always-on AI agent running on a local Linux machine).

**Key privacy concern:** The stock firmware sends device data, MAC address, and audio (via XiaoZhi AI) to external Chinese servers. The robot has a camera, dual mics, and a speaker, so this matters.

**Hardware spec:**
- ESP32-S3, 16MB flash, 8MB PSRAM
- 2.0" IPS LCD (320x240), capacitive touch
- 3x head touch pads, 9-axis IMU (BMI270)
- Dual mic (ES7210), 1W speaker (AW88298)
- 0.3MP camera (GC0308)
- 12x RGB LEDs, TTL servos (360° pan, 90° tilt)
- NFC (ST25R3916), IR Tx/Rx, MicroSD, RTC, proximity/light sensor (LTR-553ALS)
- 550mAh battery, USB-C OTG + charging

---

## Architecture (Target State)

```
iPhone (Tailscale 100.78.196.20)
    ↓ 100.78.196.20:12800
Linux machine
    ├── StackChan Go server (port 12800)
    ├── OpenClaw (always-on AI agent)
    │   └── LLM backend (Claude/GPT/local)
    ├── Whisper (local STT)
    ├── Piper/Coqui (local TTS)
    └── Tailscale
         ↑ local WiFi (192.168.x.x)
    StackChan robot
```

**Robot → Server:** WebSocket on port 12800 (audio Opus, camera JPEG, motion, heartbeat)
**iPhone → Server:** REST API + WebSocket via Tailscale
**OpenClaw → Server:** WebSocket audio subscriber + REST for control commands

---

## Progress

### ✅ Phase 1a — Security: Router block
- **TODO (Oliver):** Block outbound `47.113.125.164:12800` in router firewall
- Severs connection to M5Stack's server immediately, no code changes required
- Note: Do not use the AI Agent app until Phase 3 is complete (activates XiaoZhi)

### ✅ Phase 1b — Firmware: Redirect server URLs
**Committed: `d40b81a`**

- `firmware/main/hal/hal_account.cpp` — 3 hardcoded M5Stack IPs replaced with `YOUR_LINUX_LAN_IP` placeholder
- `firmware/main/hal/utils/secret_logic/my_override.cpp` — new file overriding weak `secret_logic` symbols to redirect WebSocket URL
- `app/lib/network/urls.dart` — app pointed at Tailscale IP `100.78.196.20:12800`

### ✅ Phase 1c — Firmware: Block OTA phone-home
The xiaozhi-esp32 submodule auto-updates itself from Aliyun on first boot (`api.tenclass.net` → `oss-cn-shenzhen.aliyuncs.com`). Blocked via:
- `firmware/sdkconfig` — `CONFIG_OTA_URL=""`
- `firmware/sdkconfig.defaults` — `CONFIG_OTA_URL=""`

The `ota.cc` guard (`if url.length() < 10 return`) silently aborts the OTA check when URL is empty.

### ✅ Firmware: Audio streaming
**Committed: `fcac305`**

Bidirectional audio streaming in `firmware/main/hal/hal_ws_avatar.cpp`:
- `StartAudioStream` / `StopAudioStream` handlers functional
- Mic capture → downsample 24kHz→16kHz → Opus encode → WebSocket send
- WebSocket receive Opus → decode → upsample 16kHz→24kHz → speaker
- Dedicated FreeRTOS task, atomic stop flag, semaphore-based shutdown

### ✅ Phase 2 — Self-hosted Go server (MacBook testbed)
Server running on MacBook at `192.168.1.240:12800`. Key changes made:

- `server/manifest/config/config.yaml` — DB, JWT, simple auth token configured
- `server/utility/rsa.go` — skip RSA init panic when all four key fields are empty
- `server/internal/web_socket/web_socket.go` — simple auth bypass: `Authorization: hi-stack-chan` + `?mac=` query param, with colon-stripping for MAC normalisation
- **Simple auth is testbed-only** — production (OpenClaw server) will use RSA keys

### ✅ Phase 3 — Firmware flashed and connecting

- Server URL: `http://192.168.1.240:12800` (in `firmware/main/hal/utils/secret_logic/my_override.cpp`)
- Robot MAC: `441BF6E55A08`
- Robot connects via WebSocket as `StackChan` device type with 12-char no-separator MAC
- Firmware built with ESP-IDF v5.5.4, flashed successfully

**Fixed during this phase:**
- Opus encoder/decoder API updated to struct-based frames (`esp_audio_enc_in_frame_t`, `esp_audio_dec_out_frame_t`)
- WebSocket URL appends 12-char MAC via `getFactoryMacString("")` (no separator)

### ✅ Remote control working — `tools/send_avatar.go`

Binary WebSocket test client at `tools/send_avatar.go`. Run from `server/`:

```bash
# Head movement (servos)
go run ../tools/send_avatar.go -mac 441BF6E55A08 -type motion -yaw 30
go run ../tools/send_avatar.go -mac 441BF6E55A08 -type motion -yaw 0 -pitch -10

# Speech bubble
go run ../tools/send_avatar.go -mac 441BF6E55A08 -type text -name Oliver -content "hello!"

# Emotion (requires reflash with json_helper.cpp fix — see below)
go run ../tools/send_avatar.go -mac 441BF6E55A08 -type emotion -emotion Angry

# Raw avatar JSON (move/resize individual features)
go run ../tools/send_avatar.go -mac 441BF6E55A08 -type avatar -json '{"leftEye":{"weight":30}}'
```

**Full command reference:**
| Type | JSON sent to robot | Effect |
|---|---|---|
| `emotion` | `{"emotion":"Happy"}` | Sets face expression |
| `motion` | `{"yawServo":{"angle":30}}` | Turns head |
| `text` | `{"name":"X","content":"Y"}` | Speech bubble for 6s |
| `avatar` | raw JSON | Move/resize eyes or mouth |

---

## ⏳ Next Session — Avatar enhancement

### Status
The robot is running the `EnhancedAvatar` skin (`firmware/main/stackchan/avatar/skins/enhanced/`):
- `EnhancedEyes` — layered circles (sclera/iris/pupil/highlight/eyelid) with per-emotion eyelid angles
- `EnhancedMouth` — coral circle base + sliding black cover, per-emotion open amount
- `EnhancedAvatar` — wires both onto a 320×240 black panel

### Pending firmware fix (needs reflash)
`firmware/main/stackchan/json/json_helper.cpp` — added `emotion` string key parsing to `update_from_json`. Without this reflash, sending `{"emotion":"Happy"}` over WebSocket does nothing (the key was simply not handled).

### Ideas for enhancement
- **Smoother emotion transitions** — lerp between eye/mouth states over ~200ms instead of snapping
- **Blink animation** — periodic random blink (eyelid sweeps down and back up), separate from emotion
- **Pupil tracking** — pupils move slightly with head yaw/pitch servo position
- **Expression detail** — eyebrows drawn as separate arcs above each eye; lift/furrow per emotion
- **Idle breathing** — very subtle vertical mouth weight oscillation at ~0.2Hz gives life when still
- **Speech animation** — mouth weight pulses while speech bubble is active (`SpeakingModifier` already exists)
- **Colour themes** — iris colour configurable (currently hardcoded coral/white)

---

## Phase 4 — Rebuild and deploy iOS app
**Can proceed now — server is running on MacBook via Tailscale**

1. `app/lib/network/urls.dart` already has Tailscale IP (`100.78.196.20:12800`)
2. Open `app/ios/Runner.xcworkspace` in Xcode
3. Set signing team (Apple Developer account)
4. Set unique bundle identifier (e.g. `com.iamboliver.stackchan`)
5. `flutter pub get && flutter build ios --release`
6. Deploy to iPhone

---

## Phase 5 — Move server to OpenClaw (Linux)

When ready to move off MacBook testbed:
1. Get Linux machine LAN IP (`ip addr`)
2. Update `firmware/main/hal/utils/secret_logic/my_override.cpp` with Linux IP
3. Copy `server/manifest/config/config.yaml` to Linux (or recreate — never in git)
4. Install Go 1.26+ and MySQL 8.0 on Linux
5. Generate proper RSA keys for production (remove simple auth)
6. Rebuild and reflash firmware with new server IP

---

## Phase 6 — OpenClaw Integration (Planned)

### 6a — OpenClaw voice terminal
**This is the foundation — everything else builds on it**

Architecture:
```
Robot mic → Opus over WebSocket
    → Go server (existing audio routing)
    → OpenClaw bridge (new component, Python)
    → Whisper (local STT) → text
    → OpenClaw → LLM → response text
    → Piper/Coqui (local TTS) → audio
    → Opus back over WebSocket
    → Robot speaker
```

Required components:
- OpenClaw skill (directory with `SKILL.md` + Python script)
- WebSocket client that subscribes to robot audio (sends `StartAudioStream` with device MAC)
- Local Whisper for STT (runs on Linux)
- Local TTS (Piper recommended for low latency)
- The Go server already handles all audio routing — no server changes needed

### 6b — Note taker
- Head tap triggers recording mode (LED change, firmware already supports touch gestures)
- Audio → Whisper → OpenClaw → structured notes + action items
- Robot nods/reacts while capturing

### 6c — Morning briefing
- Proximity sensor (LTR-553ALS) triggers on desk arrival — **requires firmware driver (not yet written)**
- As interim: head tap or time-based trigger
- OpenClaw reads calendar, weather, overnight messages → TTS → robot speaks it

### 6d — NFC personality cards
- NFC chip (ST25R3916) has zero firmware implementation — **requires driver from scratch**
- NFC tags trigger different robot modes/personas
- Each mode sets OpenClaw system prompt + robot LED colour + expression

### 6e — IR blaster
- IR Tx/Rx has zero firmware implementation — **requires driver from scratch**
- Robot learns TV/AC/light remote codes
- OpenClaw skill: "turn off the TV" → fires IR code

### 6f — Kids features
- Builds on voice terminal (6a) and NFC cards (6d)
- Story cards, emotion mirror, interactive timer, bedtime mode
- All AI filtered through kid-safe system prompt

---

## Firmware Work Remaining

| Feature | Status | Effort |
|---|---|---|
| Reflash with emotion fix (`json_helper.cpp`) | **Pending** | Trivial |
| Avatar skin enhancements | Next session | Medium |
| Proximity sensor driver (LTR-553ALS) | Not started | Medium |
| NFC driver (ST25R3916) | Not started | High |
| IR driver | Not started | High |
| IMU pick-up detection | Commented out in existing code | Low |

---

## Hardware Currently Unused

These exist on the board but have zero firmware code:
- **NFC** (ST25R3916)
- **IR** Tx/Rx
- **MicroSD** slot
- **Proximity/light sensor** (LTR-553ALS)
- **IMU pick-up detection** (commented out)

## Grove Expansion Ports

Three external Grove connectors on the body (4-pin: GND, VCC, Signal A, Signal B). Nothing uses them yet.

| Colour | Protocol | Pins | Example add-ons |
|---|---|---|---|
| Black | I2C | SDA + SCL | Environmental sensor (CO2, temp/humidity), secondary display |
| Red | GPIO | 2× digital/analog | Button, relay, simple LED trigger |
| Blue | UART | TX + RX | GPS module, RFID reader, second microcontroller |

**The three ports are not equal in power:**
- **I2C (Black)** can host *many* devices via a Grove I2C hub — most powerful, multiplexes into 6+ slots.
- **GPIO (Red)** is two pins — best for one tactile thing.
- **UART (Blue)** is one bidirectional serial peripheral — best for "smart" modules that do their own work.
- All Grove modules plug-and-play, no soldering. Low cost to try anything.

### Creative scenarios (tied to planned features)

**Tied to OpenClaw voice terminal (6a)**
- **Boss switch** (GPIO) — illuminated arcade toggle on the desk. Flip = robot face goes "do not disturb", mic mutes, OpenClaw queues notifications. Solves the real focus-time / video-call problem.
- **Air quality nag** (I2C) — SCD41 (CO2) + BME280. OpenClaw proactively says "CO2 is at 1500ppm, open a window" mid-meeting.
- **On-device vision** (UART) — HuskyLens or Grove Vision AI module does face/object recognition locally; camera frames never leave the desk. Privacy win — "Hello Oliver" without cloud.

**Tied to note taker (6b)**
- **Thermal printer** (UART) — CSN-A2L style. "Print my todos", "print today's conversation summary". Tactile, delightful, useful for hand-off to paper systems.
- **Big record button** (GPIO) — more reliable than head-tap; lights up while recording.
- **Mini secondary OLED** (I2C, shares bus with sensors) — always-on tiny status display showing live transcript or time, without main LCD power draw.

**Tied to morning briefing (6c)**
- **PIR motion sensor across the room** (GPIO) — robot knows when you've actually arrived at the desk. Triggers briefing without keeping the camera always-on. More reliable than the onboard proximity sensor (only a few cm range).
- **Sensor station on I2C bus** (I2C hub) — temp, humidity, pressure, CO2, light all chained. "Today: 21°C, stuffy, low light — open the curtains."

**Tied to NFC personality cards (6d) — alternatives if NFC driver proves hard**
- **Colour cards** (I2C) — APDS-9960 / TCS34725 colour sensor. Cardstock cards with coloured stripes. Way easier to make at home than NFC tags, kid-readable.
- **Magnetic plushies** (GPIO) — hall-effect sensor in a small dock. Toys with embedded magnets become persona tokens. Battery-free, drop-resistant, child-proof.

**Tied to IR blaster (6e)**
- **External high-power IR LED on a stalk** (GPIO) — onboard IR is short-range; an external LED array can actually reach the TV across the room.

**Tied to kids features (6f)**
- **Story scroll printer** (UART, same thermal printer as 6b) — kid picks a card, robot prints a 1-page bedtime story they can take to bed.
- **Big arcade button** (GPIO) — "press to start a story", robust enough for a 4-year-old.
- **Colour cards** (I2C, same hardware as 6d) — "find me something blue" → kid scans an object's colour with the robot.

**Cross-cutting / standalone**
- **DFPlayer mini** (UART) — dedicated MP3 module for sound effects without burdening the ESP32 audio pipeline. Robot gets a soundboard of barks, beeps, fanfares.
- **Soft potentiometer or capacitive slider** (I2C) — analog mood/volume/brightness control. More elegant than buttons for ambient adjustment.

---

## Repository

- **Fork:** `https://github.com/iamboliver/StackChan`
- **Upstream:** `https://github.com/m5stack/StackChan`
- To pull upstream updates: `git fetch upstream && git merge upstream/main`
- **Never commit:** `server/manifest/config/config.yaml` (contains DB creds + JWT secret)

## Infrastructure

- MacBook testbed IP: `192.168.1.240` (current server)
- Linux machine Tailscale IP: `100.78.196.20`
- Linux machine SSH: port `2222`
- Linux machine LAN IP: **TBD** (needed when moving server off MacBook)
- Robot MAC: `441BF6E55A08`
- Robot connects to server over local WiFi (2.4GHz only — ESP32-S3 limitation)
- iPhone connects via Tailscale (works from anywhere)
