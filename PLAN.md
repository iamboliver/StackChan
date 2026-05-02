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

**Action required:** When Linux machine is on, run `ip addr`, find `192.168.x.x`, and replace `YOUR_LINUX_LAN_IP` across both firmware files.

### ✅ Firmware: Audio streaming
**Committed: `fcac305`**

Implemented bidirectional audio streaming in `firmware/main/hal/hal_ws_avatar.cpp`:
- `StartAudioStream` / `StopAudioStream` handlers now functional
- Mic capture → downsample 24kHz→16kHz → Opus encode → WebSocket send
- WebSocket receive Opus → decode → upsample 16kHz→24kHz → speaker
- Dedicated FreeRTOS task, atomic stop flag, semaphore-based shutdown
- Green LED indicates active recording
- Destructor cleans up all codec/resampler resources

### ⏳ Phase 2 — Self-hosted Go server
**Not started — requires Linux machine to be on**

Steps:
1. Install Go 1.26+ on Linux machine
2. Install MySQL 8.0
3. Create database:
   ```bash
   mysql -u root -p -e "CREATE DATABASE stackChan CHARACTER SET utf8mb4;"
   mysql -u root -p stackChan < server/check_list/create_mysql_database.sql
   mysql -u root -p -e "CREATE USER 'stackchan'@'localhost' IDENTIFIED BY 'yourpassword'; GRANT ALL ON stackChan.* TO 'stackchan'@'localhost';"
   ```
4. Configure `server/manifest/config/config.yaml`:
   - DB connection string
   - JWT secret (`openssl rand -base64 32`)
   - Admin user credentials
   - Leave `xiaozhi` section blank
5. Build and run:
   ```bash
   cd server && go mod download
   go build -o stackchan-server main.go
   ./stackchan-server
   ```
6. Verify: `curl http://YOUR_LINUX_LAN_IP:12800/api.json`

**Note:** `server/manifest/config/config.yaml` must NOT be committed — it contains secrets. The template (blank values) is already in the repo.

### ⏳ Phase 3 — Fill in LAN IP and flash firmware
**Blocked on Phase 2**

1. Get Linux machine LAN IP (`ip addr`)
2. Replace `YOUR_LINUX_LAN_IP` in:
   - `firmware/main/hal/hal_account.cpp` (3 lines)
   - `firmware/main/hal/utils/secret_logic/my_override.cpp` (1 line)
3. Install ESP-IDF v5.5.4
4. Fetch firmware dependencies:
   ```bash
   cd firmware && python3 ./fetch_repos.py
   ```
5. Build and flash (robot connected via USB-C):
   ```bash
   idf.py set-target esp32s3
   idf.py build
   idf.py flash
   idf.py monitor  # watch logs to confirm server connection
   ```

### ⏳ Phase 4 — Rebuild and deploy iOS app
**Blocked on Phase 2**

1. `app/lib/network/urls.dart` already has Tailscale IP (`100.78.196.20:12800`)
2. Open `app/ios/Runner.xcworkspace` in Xcode
3. Set signing team (Apple Developer account)
4. Set unique bundle identifier (e.g. `com.iamboliver.stackchan`)
5. `flutter pub get && flutter build ios --release`
6. Deploy to iPhone

---

## Phase 5 — OpenClaw Integration (Planned)

### 5a — OpenClaw voice terminal
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

### 5b — Note taker
- Head tap triggers recording mode (LED change, firmware already supports touch gestures)
- Audio → Whisper → OpenClaw → structured notes + action items
- Robot nods/reacts while capturing
- "What did I say today?" → playback summary via TTS

### 5c — Morning briefing
- Proximity sensor (LTR-553ALS) triggers on desk arrival — **requires firmware driver (not yet written)**
- As interim: head tap or time-based trigger
- OpenClaw reads calendar, weather, overnight messages → TTS → robot speaks it

### 5d — NFC personality cards
- NFC chip (ST25R3916) has zero firmware implementation — **requires driver from scratch**
- NFC tags (~£5 for 50 tags) trigger different robot modes/personas
- "Work mode", "Kids mode", "Casual mode" etc.
- Each mode sets OpenClaw system prompt + robot LED colour + expression

### 5e — IR blaster
- IR Tx/Rx has zero firmware implementation — **requires driver from scratch**
- Robot learns TV/AC/light remote codes
- OpenClaw skill: "turn off the TV" → fires IR code
- Genuinely useful as office automation

### 5f — Kids features
- Builds on voice terminal (5a) and NFC cards (5d)
- Story cards: NFC tag → character/genre → OpenClaw generates story → TTS + expressions
- Emotion mirror: camera detects expression → robot mirrors it
- Interactive timer: "5 minutes for teeth brushing" → countdown with animations + celebration dance
- Bedtime mode: gradual LED dim, slowing story, robot "falls asleep"
- All AI filtered through kid-safe system prompt — no raw LLM access

---

## Firmware Work Remaining

| Feature | Status | Effort |
|---|---|---|
| Fill in LAN IP | Pending LAN IP | Trivial |
| Voice terminal audio (firmware side) | ✅ Done | — |
| Proximity sensor driver (LTR-553ALS) | Not started | Medium |
| NFC driver (ST25R3916) | Not started | High |
| IR driver | Not started | High |
| IMU pick-up detection | Commented out in existing code | Low |
| Battery face (expression from charge level) | Not started | Low |

---

## Hardware Currently Unused

These exist on the board but have zero firmware code:
- **NFC** (ST25R3916)
- **IR** Tx/Rx
- **MicroSD** slot
- **Proximity/light sensor** (LTR-553ALS)
- **IMU pick-up detection** (commented out)

---

## Repository

- **Fork:** `https://github.com/iamboliver/StackChan`
- **Upstream:** `https://github.com/m5stack/StackChan`
- To pull upstream updates: `git fetch upstream && git merge upstream/main`
- **Never commit:** `server/manifest/config/config.yaml` (contains DB creds + JWT secret)

## Infrastructure

- Linux machine Tailscale IP: `100.78.196.20`
- Linux machine SSH: port `2222`
- Linux machine LAN IP: **TBD** (needed for firmware)
- Robot connects to server over local WiFi (2.4GHz only — ESP32-S3 limitation)
- iPhone connects via Tailscale (works from anywhere)
