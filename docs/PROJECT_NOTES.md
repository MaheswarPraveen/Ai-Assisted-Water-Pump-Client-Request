# Project notes — decisions, rationale, and status

This file captures **why** the project is built the way it is, so anyone (or any AI agent) cloning the repo understands the reasoning, not just the code. Read alongside `README.md` and `wiring.md`.

---

## 1. The core problem

A household water tank is filled by a pump. Goal: control the pump intelligently — keep the tank filled, learn *when* water is typically needed, and eventually pre-fill ahead of demand — while a float switch guarantees safety.

The client's defining requirement: **"real AI" that learns on the device.** Not a cloud API, not a static pre-trained model — a network that genuinely learns from this specific household's usage, on the hardware.

## 2. Key design decisions & why

### Custom C neural network instead of TensorFlow Lite Micro
- **TFLite Micro can only run inference** on a microcontroller — it cannot train on-device.
- The client wanted the model to **learn live**. So a small MLP (`4→8→8→1`) was hand-written in C with forward pass + backprop + SGD (~120 lines). It trains continuously on the chip.
- This is a genuine neural network (layers, weights, gradient descent), just without a library. Trade-off: it's not "TensorFlow" by name, but it's the only way to get true on-device learning. (ArduTFLite was installed and verified compiling during exploration, but abandoned because it can't train live.)

### Input features: cyclical time encoding
- Inputs are `sin(2π·hour/24), cos(...), sin(2π·dow/7), cos(...)`. Cyclical encoding so 23:00≈00:00 and Sat≈Sun are "close." Output is a probability the pump is needed.
- This means the learner **needs to know the real time** → hence the NTP/WiFi requirement.

### On-device learning cadence
- Trains once per minute on `(current time → is the pump running?)`. Over days it converges to "P(pump running | this hour-of-week)".
- **Manual switching also trains it** (a deliberate manual on/off is real demand). Earlier there was a filter to ignore <1-minute manual taps; it was removed per client request so all switching registers.

### Flash persistence
- Weights + stats saved to flash (`Preferences`) every 10 min and reloaded on boot, so training survives reboots/power loss. (The `experiments/` demos are RAM-only, which is why reflashing them wipes learning — the real firmware does not.)

### WiFi as AP+STA, and the NTP routing gotcha
- The ESP32 runs its own hotspot (dashboard, always available) **and** joins home WiFi (for NTP time) simultaneously.
- **Gotcha found & fixed:** in AP+STA mode, NTP packets can try to leave via the AP interface (no gateway) and fail. Fix: force the STA as the default route (`esp_netif_set_default_netif`) and use NTP server IPs to bypass DNS. See `syncNtp()` / `setStaDefault()`.
- The dashboard is gated behind a WiFi login page so NTP (and therefore learning) is guaranteed whenever the dashboard is reachable. A separate `/wificfg` page lets you change/disconnect networks.

### Auto mode is gated behind real data
- The AI won't drive the pump until it has collected enough real samples (`AUTO_READY_SAMPLES` ≈ 3 weeks). A dataset-progress bar shows this; clicking Auto early shows a warning (enforced server-side too). Until then the float logic runs the pump.

### Dashboard chart made human-readable
- An early version showed a raw 7×24 probability grid — too cryptic. Replaced with a plain **"typical day" bar chart** + an auto-generated sentence ("Busiest around 7am and 8pm…"), and it only shows a pattern once enough real data exists (so it never presents an untrained network's noise as fact).

## 3. Hardware evolution (important — the relay/SSR/contactor story)

This is where the most back-and-forth happened; here's the resolved design:

1. **Relay (first attempt):** a cheap non-opto relay module was tried. It stuck ON because the ESP32's 3.3V isn't a clean enough logic level for its transistor. Fixed at the time with an **NPN transistor buffer** (GPIO26 → 1k → 2N2222 → relay, 10k pull-up to 5V), which **inverts the drive** — hence `RELAY_ON = HIGH` in firmware.
2. **SSR (final control device):** replaced the transistor+relay with a **Solid-State Relay (S-KAP VSR-1DA4815, DC-to-AC, 3–32VDC input, 15A)**. The ESP32's 3.3V drives it directly (terminals 3+ / 4−), no transistor. `RELAY_ON = HIGH` still correct.
3. **Contactor (for the motor):** the client supplied a **JiGO JG-1811 contactor (AC-3 18A, 240V AC coil)**. A relay/SSR shouldn't switch a motor directly — motor inrush (3–8× running current) and inductive arcing destroy contacts. The proper chain is: **ESP32 → SSR → contactor coil → contactor → motor.** The SSR switches the tiny 240V coil; the contactor (AC-3 rated) switches the heavy motor current.
4. **Direct-SSR alternative (this pump is 10A/1HP):** for a small pump the SSR *can* switch the motor directly **only with a heatsink + MCB**, accepting that an SSR fails shorted (stuck on). For 10A, a **25A** SSR is the right size (15A is too tight at 1.5× margin); the 15A is OK for short-term testing with a heatsink.
5. **Flyback diode? No.** That's for DC inductive loads. The motor is AC — a diode across an AC load shorts the mains. For AC inductive suppression use an RC snubber / MOV (often built into the SSR).
6. **Power:** a **Hi-Link HLK-5M05** (240VAC → 5V) powers the ESP32 from mains inside the box (feed VIN, not 3.3V). Isolated output = safe low-voltage side.
7. **HAND/OFF/AUTO bypass:** because if the box (ESP32/SSR) dies, the master-isolator approach leaves the pump unable to run, a **HAND/OFF/AUTO selector** is required: AUTO routes the motor live through the SSR (smart), HAND bypasses everything (direct mains to pump — works even if the box is dead, but no safety/learning), OFF disconnects.

### Manual control & the AI
- Manual on/off via the **dashboard** already registers in the AI (the ESP32 is doing the switching).
- A **physical button** can also register **if wired to a spare GPIO** (e.g. GPIO25, switch to GND, `INPUT_PULLUP`) — it "commands" the ESP32, which then drives the SSR and logs it. (Firmware hook for this is not yet added — a small TODO.)
- A **HAND-bypass** switch that powers the motor directly does **not** register (the ESP32 can't see it). To capture that you'd need a current sensor on the pump line.

## 4. Build status

**Prototype v1 (as-built):**
- ✅ ESP32 firmware: pump control + live-learning NN + dashboard + flash persistence — flashed & working
- ✅ SSR relay test passes (GPIO26 → SSR toggles)
- ✅ On-device learning proven (see `experiments/LiveLearnTest` autotest demo)
- ✅ Components assembled into an enclosure (ESP32, SSR, MCB, HLK-5M05)
- ⚠️ SSR **not yet heatsinked** (fine for bench/relay test, required before running the 10A motor continuously)
- ⚠️ Mains terminals not yet insulated/separated from low-voltage side
- ⚠️ HAND/OFF/AUTO selector, physical GPIO button, earthing, cable glands — not yet added

## 5. v2 / production roadmap

- [ ] Heatsink the SSR (mandatory for continuous 10A) — or use the contactor path so the SSR only switches the coil
- [ ] Insulate/cover all mains terminals; physically separate mains from ESP32 + float wiring
- [ ] Add the **HAND / OFF / AUTO** bypass selector (box-failure fallback)
- [ ] Add the physical **GPIO25 manual button** (+ firmware hook) so hardware manual control also feeds the AI
- [ ] Cable glands + strain relief; earth terminal for the pump
- [ ] Secure the HLK-5M05
- [ ] (Optional) RC snubber/MOV across the motor if any SSR misbehavior appears
- [ ] (Optional) current sensor to detect pump running in HAND mode

## 6. Notes for the next developer / AI agent

- The **production firmware is `firmware/SmartPump/SmartPump.ino`**. The `experiments/` sketches are demos/tests; don't confuse them for the deployed code.
- `RELAY_ON = HIGH` is correct for the SSR (and was also correct for the transistor-buffered relay). If you ever wire a plain active-LOW relay directly, flip it.
- The learner does nothing without a valid clock — if predictions sit near 50% and "dataset progress" doesn't climb, check that NTP time is being obtained (needs home WiFi configured via the dashboard).
- Reflashing the **experiments** wipes their RAM-only network; the **real firmware persists** to flash and survives reflash/reboot.
- Board: DOIT ESP32 DEVKIT V1. On Windows, close the Arduino IDE Serial Monitor before flashing or the COM port is locked ("Access is denied").
