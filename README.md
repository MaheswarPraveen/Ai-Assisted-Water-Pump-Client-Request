# AI-Assisted Water Pump 💧⚡ (Client Project)

> **This was built as a client project — not an original personal concept.** The client's headline requirement was **"real AI"**: a genuine neural network that **learns on the device**, not a cloud service and not a pre-baked model. This repo is the full implementation: firmware, on-device machine learning, a web dashboard, and the hardware/wiring design.

An ESP32 controls a water-tank fill pump. A **hand-written neural network runs and *trains* directly on the ESP32** (no TensorFlow, no cloud) and learns the household's water-usage pattern from real pump on/off events. A float switch provides the safety backstop at all times. Once trained, it can automate filling ahead of demand.

---

## 📌 If you just cloned this — read this first

This project was built iteratively with a lot of hardware in the loop. To understand **everything that was done and why**, read these in order:
1. **This README** — what it is, architecture, how to build/run
2. **[docs/wiring.md](docs/wiring.md)** — the full hardware wiring (ESP32 → SSR → contactor → motor, power, safety)
3. **[docs/PROJECT_NOTES.md](docs/PROJECT_NOTES.md)** — the design decisions, *why* each choice was made, current build status (prototype v1), and the v2 roadmap

The actual deployed firmware is **[firmware/SmartPump/SmartPump.ino](firmware/SmartPump/SmartPump.ino)**. The `experiments/` folder holds the throwaway test sketches used to prove out each piece.

---

## What makes it "real AI" (the client's requirement)

- **Hand-written neural network in plain C** — `4 → 8 → 8 → 1`, with the forward pass, **backpropagation, and gradient descent all implemented from scratch** (~120 lines). No TensorFlow / TFLite / any ML library.
- **Trains live, on the chip** — this is the key point. TensorFlow Lite Micro can only *run* a frozen model on a microcontroller; it can't train. Because we wrote the backprop ourselves, this network **learns continuously on-device** from real events — exactly the "AI that learns as it goes" the client wanted.
- **Learns your actual pattern** — inputs are the cyclical time-of-week (`sin/cos` of hour and day). Every real pump on/off event (automatic *or* manual) is a training example: *"at this hour, on this day, was the pump running?"* Over days/weeks it learns when water is needed.
- **Persists to flash** — trained weights + usage stats survive reboots and power loss.

## Features

- **Float-driven pump control** — tank low → pump on, satisfied → pump off, plus a max-runtime safety cutoff (dry-source / stuck-float protection). Works even with no WiFi.
- **Live on-device learning** — as above; manual switching also counts as training data.
- **Web dashboard** (dark glassmorphism) served from the ESP32's own WiFi hotspot: live status, activity timeline, usage stats, a plain-English "typical day" chart, predictions, manual ON/OFF/Auto controls, fault log, CSV export.
- **WiFi login flow** — connects to home WiFi (live, no reboot) purely to get accurate time via NTP, which the learner needs.
- **Auto mode gated** — the AI won't take over the pump until it has collected enough real data (dataset progress bar; unlocks ~3 weeks).
- **Manual override** (ON / OFF / Auto), safety-guarded.

## Hardware (bill of materials)

| Part | Spec / model | Role |
|---|---|---|
| ESP32 DevKitV1 | — | the brain + neural net |
| Float switch | tethered/cable type | tank level (single float gives low + full via tether) |
| Solid-State Relay | S-KAP **VSR-1DA4815** (DC-to-AC, 3–32VDC in, 24–480VAC, 15A) | switches the pump / contactor coil |
| Contactor | JiGO **JG-1811** (AC-3 18A, **240V AC coil**) | heavy motor switch (for larger pumps) |
| 5V PSU | Hi-Link **HLK-5M05** (100–240VAC → 5V 1A) | powers the ESP32 from mains inside the box |
| MCB | 16A | pump-line protection |
| Heatsink | aluminium | **mandatory** on the SSR at motor current |
| HAND/OFF/AUTO selector | mains-rated 3-pos | manual bypass if the box fails |
| Pump | 1 HP single-phase, ~10A, 240V (this build) | the load |

### Pinout

| Signal | GPIO |
|---|---|
| Float switch | 25 (`INPUT_PULLUP`) |
| Relay/SSR control | 26 (HIGH = pump ON) |
| Status LED | 2 (onboard blue) |

Full circuit + mains/safety wiring: **[docs/wiring.md](docs/wiring.md)**.

## Getting started

1. Open `firmware/SmartPump/SmartPump.ino` in Arduino IDE. Install the **ESP32 board package**; select **DOIT ESP32 DEVKIT V1**.
2. Flash it. Connect to the **`SmartPump`** WiFi hotspot (password `pump12345`).
3. Browse to `http://192.168.4.1` — a WiFi setup page appears; enter your home WiFi (needed so the device gets accurate time for the learner).
4. The dashboard loads. Pump control works immediately (float-driven); the AI accumulates data over time and unlocks Auto mode once it has enough.

## Repo layout

```
firmware/SmartPump/        ← the real deployed firmware
experiments/               ← dev test rigs (proof-of-concept sketches)
  RelayTest/                 minimal relay/SSR on-off toggle
  TinyNetTest/               NN trains on synthetic data, prints an ASCII heatmap over serial
  LiveLearnTest/             fast live-learning demo (autotest on a short repeating cycle + relay playback)
docs/
  wiring.md                  full hardware wiring + safety
  PROJECT_NOTES.md           design decisions, rationale, build status, v2 roadmap
```

## Safety (short version — read docs/wiring.md before touching mains)

- The relay/contactor **NO (normally-open)** path is used → a dead/crashed board defaults to **pump OFF**.
- Max-runtime cutoff protects against dry-run / stuck float.
- Mains AC (pump power) is a separate, isolated circuit from the ESP32 low-voltage side.
- SSR **must** be heatsinked at motor current. Add an MCB on the pump line. Earth the pump.
- HAND/OFF/AUTO selector gives a manual bypass so a failed controller never leaves you without water.
