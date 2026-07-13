# Wiring guide (final design)

## ⚠️ Safety first
The pump side is **mains AC (240V)** — genuinely dangerous. If you're not confident with mains, have an electrician do the pump-side connections. The ESP32 / SSR-control side is low-voltage DC and safe.

---

## Control device: Solid-State Relay (SSR)
Final build uses an **S-KAP VSR-1DA4815** — DC-to-AC SSR, **control 3–32VDC**, output **24–480VAC 15A**. The ESP32's 3.3V drives it **directly**, no transistor/relay needed.

**SSR terminals:**
- `1` & `2` (top) = **OUTPUT** (AC, in series with the load)
- `3 (+)` & `4 (−)` (bottom) = **CONTROL** (DC, from the ESP32)

**Control-side wiring (ESP32 → SSR):**
```
ESP32 GPIO26 → SSR terminal 3 (+)
ESP32 GND    → SSR terminal 4 (−)
```
`GPIO26 HIGH → SSR on`. Firmware: `RELAY_ON = HIGH`. No code change vs the earlier transistor+relay build.

> Historical note: an earlier build used a non-opto relay + NPN transistor buffer because the relay stuck on at 3.3V. The SSR replaces that entire stage. The `RELAY_ON = HIGH` polarity is unchanged.

---

## Two ways to switch the motor

### Option A — SSR → contactor → motor (robust, for larger pumps)
The SSR switches the **contactor coil**; the contactor (AC-3 rated) switches the motor. Best for motors where inrush would damage an SSR.

Contactor: **JiGO JG-1811**, coil marked **`Uc 240V 50/60Hz`** → drive the coil straight from mains through the SSR.

```
Control:
  Mains Live → SSR term 1
  SSR term 2 → contactor coil A1
  contactor coil A2 → Mains Neutral        (SSR on → 240V across coil → contactor pulls in)

Power:
  Mains Live → contactor L1
  contactor T1 → Pump Live
  Mains Neutral → Pump Neutral (direct)     (single-phase: one pole; leave L2/L3, T2/T3 unused)
```

### Option B — SSR direct to motor (small pump only, e.g. this 1HP/10A build)
Only if the pump's running current is comfortably under the SSR rating. For 10A use a **25A** SSR (15A is too tight for continuous motor duty; OK for short-term testing). **A heatsink is mandatory.**

```
Mains Live → [16A MCB] → SSR term 1
SSR term 2 → Pump Live
Mains Neutral → Pump Neutral (direct)
Earth → Pump body
```

**Hard requirements for Option B:**
- **Heatsink** bolted to the SSR (thermal paste) — ~15W at 10A, it will fail bare
- **16A MCB** on the live line (fault / stalled-motor protection)
- Accept that an SSR fails **shorted (stuck ON)** — the MCB is the backstop
- **No flyback diode** (that's for DC loads; a diode across an AC load shorts the mains). Use an RC snubber / MOV only if the SSR misbehaves.

---

## Powering the ESP32 from mains — Hi-Link HLK-5M05
`100–240VAC → 5V 1A`. Powers the ESP32 inside the box, no USB adapter needed.

```
INPUT (AC pins, no polarity):
  Mains Live    → AC
  Mains Neutral → AC
OUTPUT:
  +Vo → ESP32 VIN (5V pin — NOT 3.3V)
  -Vo → ESP32 GND
```
- Put a small **fuse (~0.5A)** on the AC input.
- Output is isolated → the ESP32 5V/GND side is safe low-voltage.
- Only power the ESP32 from **one** source at a time (mains-5V OR USB while programming).

---

## HAND / OFF / AUTO bypass (so a failed box never leaves you without water)
A mains-rated 3-position **changeover** selector on the motor's live line:

```
             ┌─ HAND → Mains Live direct to Pump   (works even if ESP32/SSR dead; manual only)
Mains Live ──┼─ OFF  → open                         (pump off)
             └─ AUTO → through the SSR              (ESP32: smart control + safety + learning)
Mains Neutral ─────────→ Pump (direct)
```
Because it's a changeover, HAND and AUTO are never connected at once (no parallel-bypass problem). In **AUTO** the ESP32 has full control; **HAND** is the dead-box fallback (no float/max-runtime safety, doesn't feed the AI).

---

## Float switch (level sensor)
```
Float switch → ESP32 GPIO25   (INPUT_PULLUP)
Float switch → ESP32 GND
```
Two wires, low-voltage. A single tethered float gives both "low" and "full" via its tether slack. Firmware `FLOAT_CALL_LEVEL` sets which level means "water low" (flip if reversed).

## Optional physical manual button (registers in the AI)
```
Button → ESP32 GPIO25   (INPUT_PULLUP)
Button → ESP32 GND
```
Low-voltage signal (3.3V, microamps) — the ESP32 reads it and drives the SSR, so it's logged for learning. (Firmware hook not yet added — see PROJECT_NOTES v2 roadmap.)

---

## Full picture (Option B, this 1HP build)
```
   LOW VOLTAGE (safe)                    |     MAINS 240V (dangerous, enclose)
  HLK-5M05 → 5V → ESP32 VIN/GND          |   Live → HAND/OFF/AUTO selector
  GPIO25 ← float switch                  |        AUTO → [MCB] → SSR t1 → t2 → Pump Live
  GPIO26 → SSR t3 (+)                    |        HAND → Pump Live (direct)
  GND    → SSR t4 (−)                    |   Neutral → Pump (direct)
  GPIO25 ← optional manual button        |   Earth → Pump
```

## Enclosure checklist
- [ ] SSR on a heatsink, with airflow
- [ ] All mains terminals insulated/covered, physically separated from ESP32 + float wiring
- [ ] HLK-5M05 secured; fuse on its AC input
- [ ] Cable glands (strain relief) for every wire entering/leaving
- [ ] Earth terminal for the pump
- [ ] MCB on the pump live line
