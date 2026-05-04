// WebSocket test client for StackChan server.
// Run from the server directory: go run ../tools/send_avatar.go [flags]
//
// Examples:
//   go run ../tools/send_avatar.go -mac 441BF6E55A08 -type emotion -emotion Happy
//   go run ../tools/send_avatar.go -mac 441BF6E55A08 -type motion -yaw 30 -pitch -10
//   go run ../tools/send_avatar.go -mac 441BF6E55A08 -type text -name Oliver -content "hello!"
//   go run ../tools/send_avatar.go -mac 441BF6E55A08 -type avatar -json '{"leftEye":{"weight":50}}'
//
//go:build ignore

package main

import (
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"strings"

	"github.com/gorilla/websocket"
)

const (
	typeControlAvatar byte = 0x03
	typeControlMotion byte = 0x04
	typeTextMessage   byte = 0x07
)

func buildPacket(msgType byte, payload []byte) []byte {
	buf := make([]byte, 1+4+len(payload))
	buf[0] = msgType
	binary.BigEndian.PutUint32(buf[1:5], uint32(len(payload)))
	copy(buf[5:], payload)
	return buf
}

func mustJSON(v any) []byte {
	b, err := json.Marshal(v)
	if err != nil {
		log.Fatalf("json: %v", err)
	}
	return b
}

func main() {
	server  := flag.String("server", "ws://192.168.1.240:12800", "WebSocket server base URL")
	mac     := flag.String("mac", "", "12-char hex MAC, e.g. 441BF6E55A08")
	token   := flag.String("token", "hi-stack-chan", "Simple auth token")
	msgType := flag.String("type", "emotion", "Message type: emotion | motion | text | avatar")

	// emotion flags
	emotion := flag.String("emotion", "Happy", "Emotion: Neutral Happy Angry Sad Doubt Sleepy")
	bgcolor := flag.String("bgcolor", "", "Background colour hex e.g. #D0E8FF (optional, works with any type)")

	// motion flags
	yaw   := flag.Int("yaw", 0, "Yaw servo angle (-90..90)")
	pitch := flag.Int("pitch", 0, "Pitch servo angle (-90..90)")

	// text flags
	name    := flag.String("name", "Server", "Speaker name for TextMessage")
	content := flag.String("content", "hello!", "Message content for TextMessage")

	// raw avatar JSON override
	avatarJSON := flag.String("json", "", "Raw JSON payload for ControlAvatar (overrides -emotion)")

	flag.Parse()

	if *mac == "" {
		log.Fatal("-mac is required")
	}
	macNorm := strings.ReplaceAll(*mac, ":", "")
	if len(macNorm) != 12 {
		log.Fatalf("MAC must be 12 hex chars (got %d): %s", len(macNorm), macNorm)
	}

	var pktType byte
	var jsonPayload []byte

	// mergeColor injects bgColor into an avatar payload map when -bgcolor is set
	mergeColor := func(body map[string]any) {
		if *bgcolor != "" {
			body["bgColor"] = *bgcolor
		}
	}

	switch *msgType {
	case "emotion":
		pktType = typeControlAvatar
		body := map[string]any{"emotion": *emotion}
		if *avatarJSON != "" {
			// parse raw JSON and merge bgcolor on top
			if err := json.Unmarshal([]byte(*avatarJSON), &body); err != nil {
				log.Fatalf("invalid -json: %v", err)
			}
		}
		mergeColor(body)
		jsonPayload = mustJSON(body)

	case "avatar":
		pktType = typeControlAvatar
		if *avatarJSON == "" {
			log.Fatal("-json is required for type=avatar")
		}
		body := map[string]any{}
		if err := json.Unmarshal([]byte(*avatarJSON), &body); err != nil {
			log.Fatalf("invalid -json: %v", err)
		}
		mergeColor(body)
		jsonPayload = mustJSON(body)

	case "motion":
		pktType = typeControlMotion
		body := map[string]any{}
		if *yaw != 0 {
			body["yawServo"] = map[string]int{"angle": *yaw}
		}
		if *pitch != 0 {
			body["pitchServo"] = map[string]int{"angle": *pitch}
		}
		if len(body) == 0 {
			log.Fatal("motion type requires at least -yaw or -pitch")
		}
		jsonPayload = mustJSON(body)

	case "text":
		pktType = typeTextMessage
		jsonPayload = mustJSON(map[string]string{"name": *name, "content": *content})

	default:
		log.Fatalf("unknown -type %q (emotion | motion | text | avatar)", *msgType)
	}

	// All App→robot packets are prefixed with the 12-byte MAC
	payload := append([]byte(macNorm), jsonPayload...)
	packet := buildPacket(pktType, payload)

	// Connect as App client
	u, _ := url.Parse(*server + "/stackChan/ws")
	q := u.Query()
	q.Set("deviceType", "App")
	q.Set("mac", macNorm)
	q.Set("deviceId", "test-tool-001")
	u.RawQuery = q.Encode()

	hdr := http.Header{}
	hdr.Set("Authorization", *token)

	conn, resp, err := websocket.DefaultDialer.Dial(u.String(), hdr)
	if err != nil {
		log.Fatalf("dial: %v (HTTP %v)", err, resp)
	}
	defer conn.Close()

	_, banner, _ := conn.ReadMessage()
	fmt.Printf("Server: %s\n", banner)

	if err := conn.WriteMessage(websocket.BinaryMessage, packet); err != nil {
		log.Fatalf("write: %v", err)
	}
	fmt.Printf("Sent type=0x%02X mac=%s payload=%s (%d bytes total)\n",
		pktType, macNorm, jsonPayload, len(packet))
}
