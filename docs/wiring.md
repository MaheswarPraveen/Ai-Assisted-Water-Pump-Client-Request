# Wiring guide

## ⚠️ Safety first
This involves **mains AC voltage** on the pump side — genuinely dangerous if done wrong (shock/fire risk). If you're not confident handling live mains wiring, have an electrician do the pump-side connections. Everything described as the "ESP32 side" below is low-voltage DC and safe to wire yourself; only the pump/relay-contact side touches mains.

## Two separate circuits — never mix them

```
┌─────────────── LOW VOLTAGE (safe, DC) ───────────────┐   ┌──────── MAINS AC (dangerous) ────────┐
│                                                        │   │                                        │
│  Float switch ──GPIO32                                │   │   Wall AC ──┬── Pump (Live)            │
│                                                        │   │             │                          │
│  ESP32 ──GPIO26──[1k]──[NPN]──[relay IN]               │   │             └── relay COM              │
│                    │      │                            │   │                   │                    │
│                  emitter  10k pull-up to VIN(5V)        │   │              relay NO ── Pump (Live)   │
│                    │                                    │   │                                        │
│                   GND                                   │   │   Wall AC(Neutral) ── Pump (Neutral)  │
│                                                        │   │        (goes straight to pump, never    │
│  Relay VCC ── VIN(5V)   Relay GND ── GND               │   │         through the relay)              │
└────────────────────────────────────────────────────────┘   └────────────────────────────────────────┘
```

**Key rule:** the relay's **contacts** (COM/NO) are the only bridge between the two sides, and they're physically isolated — no electrical connection between the ESP32's 5V/GND and the mains wiring passes through them.

## Why a transistor buffer is needed
Many cheap relay modules (non-opto-isolated) need a cleaner/stronger 5V signal than the ESP32's 3.3V logic reliably provides, and can otherwise stick on or fail to release. The buffer below fixes that:

```
GPIO26 ──[1kΩ]── NPN base (e.g. 2N2222)
                  NPN emitter ── GND
                  NPN collector ── relay IN
VIN(5V) ──[10kΩ]── relay IN     (pull-up: holds IN high = relay off by default)
Relay VCC ── VIN(5V)
Relay GND ── GND
```

This inverts the logic: **GPIO26 HIGH → relay ON** (already reflected in the firmware: `RELAY_ON = HIGH`).

## Step by step (pump side)

1. **Neutral wire**: straight from the wall outlet/junction to the pump — **never interrupted by the relay**.
2. **Live wire**: cut it — one end to relay **COM**, the pump's live end to relay **NO** (normally-open, so power is off by default — fail-safe if the board loses power or crashes).
3. **Enclosure**: put the mains-side splice (COM/NO/pump live) in a proper junction box or the relay's own enclosure — never leave bare mains wire exposed.
4. **Check the pump's rated current** against the relay's rating (many small relay boards are rated ~10A @250VAC). If the pump draws more, have the relay switch a **contactor** instead of the pump directly — the contactor handles the heavy current, the relay just triggers the contactor's coil.
5. **Optional physical HAND/OFF/AUTO switch**: put a 3-position selector on the pump's live wire *before* the relay — HAND bypasses the relay entirely (direct to pump, works even if the ESP32 is off/crashed), OFF cuts power completely, AUTO routes through the relay as wired above.

## Powering the ESP32
Use a standard **5V USB wall adapter** (phone charger) plugged into a normal outlet — a separate, isolated supply from the pump's mains circuit. No electrical connection is needed between the two beyond both being plugged into building wiring.

## Summary
- Only the pump's **Live** wire passes through the relay contacts
- **Neutral** never touches the relay
- ESP32 gets its own simple USB power supply, isolated from the pump circuit
- If in doubt about the mains splice, get an electrician for that part
