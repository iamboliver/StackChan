#!/usr/bin/env python3
"""
StackChan STT bridge — subscribes to robot audio, decodes Opus, transcribes with Whisper.

Setup:
    brew install opus
    pip install openai-whisper websocket-client opuslib webrtcvad numpy

Usage:
    python tools/stt_bridge.py
    python tools/stt_bridge.py --model medium --silence 0.8

Tail the log:
    tail -f server/logs/stt.log
"""

import argparse, os, queue, struct, threading, time
import numpy as np
import opuslib
import webrtcvad
import whisper
import websocket

# ── Protocol constants ────────────────────────────────────────────────────────
TYPE_OPUS     = 0x01
TYPE_ON_AUDIO = 0x18

# ── Audio constants ───────────────────────────────────────────────────────────
SAMPLE_RATE       = 16_000
OPUS_FRAME_SIZE   = 960   # samples — 60 ms at 16 kHz (matches firmware encoder)
VAD_FRAME_MS      = 30
VAD_FRAME_SAMPLES = SAMPLE_RATE * VAD_FRAME_MS // 1000   # 480
VAD_FRAME_BYTES   = VAD_FRAME_SAMPLES * 2                # 960 bytes int16 LE

LOG_PATH = os.path.join(os.path.dirname(__file__), "..", "server", "logs", "stt.log")


# ── Logging ───────────────────────────────────────────────────────────────────

_log_file = None

def _init_log(path):
    global _log_file
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    _log_file = open(path, "a", buffering=1)  # line-buffered

def log(step: str, msg: str = ""):
    ts   = time.strftime("%Y-%m-%dT%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"
    line = f"{ts} [{step}] {msg}"
    print(line, flush=True)
    if _log_file:
        print(line, file=_log_file, flush=True)


# ── Protocol helpers ──────────────────────────────────────────────────────────

def build_packet(msg_type: int, payload: bytes) -> bytes:
    return struct.pack(">BI", msg_type, len(payload)) + payload

def parse_packet(data: bytes):
    if len(data) < 5:
        return None, b""
    msg_type = data[0]
    length   = struct.unpack(">I", data[1:5])[0]
    return msg_type, data[5:5 + length]


# ── Bridge ────────────────────────────────────────────────────────────────────

class STTBridge:
    def __init__(self, server, mac, token, model_name, silence_s, vad_level):
        self.server    = server
        self.mac       = mac.replace(":", "")
        self.token     = token
        self.silence_s = silence_s

        log("INIT", f"Loading Whisper model='{model_name}'")
        t0 = time.time()
        self.model = whisper.load_model(model_name)
        log("INIT", f"Whisper ready in {time.time()-t0:.1f}s")

        self.decoder     = opuslib.Decoder(SAMPLE_RATE, 1)
        self.vad         = webrtcvad.Vad(vad_level)
        self.q           = queue.Queue()
        self._first_frame = None

    # ── WebSocket callbacks ───────────────────────────────────────────────────

    def _on_open(self, ws):
        log("CONNECT", f"subscribing to audio for {self.mac}")
        ws.send(build_packet(TYPE_ON_AUDIO, self.mac.encode()), websocket.ABNF.OPCODE_BINARY)

    def _on_message(self, ws, data):
        if not isinstance(data, (bytes, bytearray)):
            return
        msg_type, payload = parse_packet(data)
        if msg_type == TYPE_OPUS and payload:
            t_recv = time.time()
            if self._first_frame is None:
                self._first_frame = t_recv
                log("AUDIO", "first Opus frame received — audio stream active")
            try:
                pcm = self.decoder.decode(bytes(payload), OPUS_FRAME_SIZE)
                self.q.put((pcm, t_recv))
            except Exception:
                pass  # occasional malformed frames; skip silently

    def _on_error(self, ws, err):
        log("ERROR", str(err))

    def _on_close(self, ws, code, msg):
        log("DISCONNECT", f"code={code}")

    # ── VAD + Whisper loop ────────────────────────────────────────────────────

    def _transcribe_loop(self):
        silence_limit = int(self.silence_s * 1000 / VAD_FRAME_MS)

        buf           = b""
        utterance     = b""
        silence_count = 0
        in_speech     = False
        speech_start  = None

        while True:
            try:
                pcm, t_recv = self.q.get(timeout=1.0)
            except queue.Empty:
                continue

            buf += pcm

            while len(buf) >= VAD_FRAME_BYTES:
                frame, buf = buf[:VAD_FRAME_BYTES], buf[VAD_FRAME_BYTES:]
                speech = self.vad.is_speech(frame, SAMPLE_RATE)

                if speech:
                    if not in_speech:
                        speech_start = time.time()
                        log("SPEECH_START", "voice activity detected")
                        in_speech = True
                    silence_count = 0
                    utterance += frame
                elif in_speech:
                    utterance += frame
                    silence_count += 1
                    if silence_count >= silence_limit:
                        speech_end = time.time()
                        duration   = len(utterance) / 2 / SAMPLE_RATE
                        log("SPEECH_END", f"utterance={duration:.2f}s  "
                                          f"vad_wall={speech_end - speech_start:.2f}s")
                        self._run_whisper(utterance, speech_start)
                        utterance, silence_count, in_speech, speech_start = b"", 0, False, None

    def _run_whisper(self, pcm: bytes, speech_start: float):
        audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
        secs  = len(audio) / SAMPLE_RATE
        if secs < 0.4:
            log("WHISPER_SKIP", f"utterance too short ({secs:.2f}s) — likely noise")
            return

        log("WHISPER_START", f"audio={secs:.2f}s  queue_depth={self.q.qsize()}")
        t0     = time.time()
        result = self.model.transcribe(audio, language="en", fp16=False)
        text   = result["text"].strip()
        t_done = time.time()

        whisper_ms = int((t_done - t0) * 1000)
        total_ms   = int((t_done - speech_start) * 1000)

        if text:
            log("WHISPER_DONE", f"whisper={whisper_ms}ms  total={total_ms}ms")
            log("TRANSCRIPT",   f'"{text}"')
        else:
            log("WHISPER_DONE", f"empty result  whisper={whisper_ms}ms")

    # ── Entry point ───────────────────────────────────────────────────────────

    def run(self):
        threading.Thread(target=self._transcribe_loop, daemon=True).start()
        url = (f"{self.server}/stackChan/ws"
               f"?deviceType=App&mac={self.mac}&deviceId=stt-bridge-001")
        websocket.WebSocketApp(
            url,
            header={"Authorization": self.token},
            on_open=self._on_open,
            on_message=self._on_message,
            on_error=self._on_error,
            on_close=self._on_close,
        ).run_forever()


def main():
    ap = argparse.ArgumentParser(description="StackChan STT bridge")
    ap.add_argument("--server",  default="ws://192.168.1.240:12800")
    ap.add_argument("--mac",     default="441BF6E55A08")
    ap.add_argument("--token",   default="hi-stack-chan")
    ap.add_argument("--model",   default="small",
                    choices=["tiny", "base", "small", "medium", "large"])
    ap.add_argument("--silence", default=0.6, type=float,
                    help="Seconds of silence that ends an utterance (default 0.6)")
    ap.add_argument("--vad",     default=2, type=int, choices=[0, 1, 2, 3],
                    help="VAD aggressiveness: 0=permissive 3=strict (default 2)")
    ap.add_argument("--log",     default=LOG_PATH,
                    help="Log file path (default: server/logs/stt.log)")
    args = ap.parse_args()

    _init_log(args.log)
    log("START", f"server={args.server} mac={args.mac} model={args.model} "
                 f"silence={args.silence}s vad={args.vad} log={args.log}")

    STTBridge(args.server, args.mac, args.token, args.model, args.silence, args.vad).run()


if __name__ == "__main__":
    main()
