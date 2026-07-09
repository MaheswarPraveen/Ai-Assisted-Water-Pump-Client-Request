# SmartPump 

> Built as a client project (not an original personal concept) — implemented end-to-end here: firmware, on-device ML, dashboard, and hardware integration.

A tank-fill pump controller for the ESP32 with a **real, hand-written neural network that learns your household's water usage pattern live, on the device** — no cloud, no external ML library. It predicts when the pump is likely to be needed and (once trained) can automate filling ahead of demand, with a float switch as the safety backstop the whole time.

## Features

- **Float-driven pump control** — low → pump on, satisfied → pump off, with a max-runtime safety cutoff (protects against a dry source or stuck float)
- **On-device neural network** (4→8→8→1, hand-written forward pass + backprop + SGD in plain C) — trains live from real pump on/off events, no TensorFlow/TFLite, no PC required
- **Flash persistence** — trained weights and usage stats survive reboots and power loss
- **Web dashboard** (dark glassmorphism UI) served from the ESP32's own WiFi hotspot: live status, activity timeline, usage stats, learned-pattern chart, predictions, manual ON/OFF/Auto controls, fault log, CSV export
- **WiFi login flow** — connects to your home network live (no reboot) to get accurate time via NTP, which the learner needs to know *when* it is
- **Manual override** (ON/OFF/Auto) — safety-guarded; forcing the pump on still respects the float and max-runtime cutoff

## Hardware

| Part | Notes |
|---|---|
| ESP32 DevKitV1 | any ESP32 dev board works |
| Float switch (tethered/cable type) | single float gives both "low" and "satisfied" via tether slack |
| 5V relay module | needs a driver buffer (see below) if it's a non-isolated board |
| NPN transistor (e.g. 2N2222) + 1kΩ + 10kΩ resistors | buffers 3.3V logic to a clean 5V relay drive |

### Pinout

| Signal | GPIO |
|---|---|
| Float switch | 32 (`INPUT_PULLUP`) |
| Relay control (via transistor buffer) | 26 |
| Status LED | 2 (onboard) |

See [`docs/wiring.md`](docs/wiring.md) for the full transistor-buffer circuit and mains/pump wiring diagram, including safety notes for the AC side.

## Getting started

1. Open `firmware/SmartPump/SmartPump.ino` in Arduino IDE (ESP32 board package required — board: **DOIT ESP32 DEVKIT V1**)
2. Flash it, then connect to the **`SmartPump`** WiFi hotspot it creates (password `pump12345`)
3. Browse to `http://192.168.4.1` — you'll land on a WiFi setup page; enter your home WiFi so the device can get accurate time (needed for the learner)
4. Once connected, the dashboard loads. Pump control works immediately (float-driven); the AI needs real usage data to accumulate before Auto mode unlocks

## How the learning works

Every real pump on/off event is a training example: `(hour of day, day of week) → was the pump running?`. The network trains on real events roughly once a minute, live, with weights saved to flash periodically. There's no synthetic/seed data — it starts knowing nothing and learns purely from your actual usage over time. Manual test presses under a minute are excluded from training so accidental taps don't pollute the dataset.

## Repo layout

```
firmware/SmartPump/     the real deployed firmware
experiments/            throwaway test rigs used during development
  RelayTest/             minimal relay on/off test
  TinyNetTest/           proof-of-concept: NN trains on synthetic data, prints an ASCII heatmap over serial
  LiveLearnTest/         fast-iteration rig for testing the live-learning loop on a short repeating cycle
```

## Safety notes

- The relay's **NO (normally-open)** contact is used, so a dead/crashed/unplugged board defaults to **pump off**
- Max-runtime cutoff protects against a dry source or a stuck float
- Mains AC wiring (the pump's power line) is a separate, isolated circuit from the ESP32's low-voltage side — see `docs/wiring.md` before wiring a real pump
