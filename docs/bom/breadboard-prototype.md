# ECU-25 — breadboard prototype BOM and build order

The production board is SMD (0603 passives, SOIC-8 analog, LQFP-100 MCU) and
carries 415 V field wiring — none of that belongs on a breadboard. This sheet
is the **prototype translation**: through-hole substitutes with the same
electrical behaviour, in eight blocks you can build and test one at a time,
each proving one part of [docs/circuits/README.md](../circuits/README.md).

Every block links to its reference schematic in [docs/circuits/](../circuits/),
and lists its nodes explicitly — a parts table alone is not enough to wire the
analog blocks correctly.

**Spreadsheet version:** [ecu25-breadboard-bom.xlsx](ecu25-breadboard-bom.xlsx)
— the same content as sheets you can take to the bench and the shop: a
consolidated shopping list with spares and a tick column, one tab per block,
and the safety rules up front. It is generated from this file
(`hardware/kicad/gen/gen_breadboard_xlsx.py`), so edit the markdown, not the
workbook.

Production part list: [ecu25-main-bom.md](ecu25-main-bom.md) /
[ecu25-main-bom.csv](ecu25-main-bom.csv).

---

## Safety

Read this before blocks F and G. Everything else runs at 12 V or below.

### The 415 V AC sensing does not go on a breadboard

The AC voltage sense divider connects to line voltage. At 415 V line-to-line, a
breadboard's 3–5 mm contact spacing and exposed jumper wires are lethal — that
spacing is below what insulation standards require even for the PCB, which is
why the real board uses 8-pole terminal blocks with alternate poles left empty.

Block G is therefore specified **only** against a 12 V AC wall transformer,
with the divider rescaled so the ADC sees the same signal. That validates the
bias network, the filter, the ADC scaling and the firmware's RMS and frequency
maths. Validation at real line voltage happens on the assembled PCB, in an
enclosure, by someone qualified to work on the genset panel.

### Grounding — two different rules, and they are not the same

**On the breadboard (block G):** bond one leg of the 12 V transformer's
secondary to breadboard ground. This is *required*, not forbidden — the
secondary is galvanically isolated from mains, so tying it to your DC ground
is safe, and the divider needs that return path. Without the bond, the
transformer output floats and the reading is meaningless.

**On the assembled board:** the netlist puts J5 pin 4 and J6 pin 4 (the GEN and
MAINS neutrals) on board GND. **The ECU's ground is therefore at mains neutral
potential, and the board is not isolated from mains.** That ground is shared by
the battery negative, the CT circuit, the debug UART and the SWD header. During
commissioning with AC sense live, use a battery-powered scope or a differential
probe, an isolated ST-Link, and no mains-earthed laptop. With a lost or swapped
neutral, that "ground" sits at line potential.

### Current transformers

**Never open-circuit a live CT.** A CT with current in the primary and no
burden across its secondary develops hundreds of volts. This is why block F
below has the burden resistor soldered directly to the CT leads and *not*
plugged into the breadboard: a jumper that works loose is exactly that open
circuit. If any breadboard contact can interrupt the CT loop, the circuit is
wired wrong.

### Supply protection

Set the bench supply's **current limit to 0.5 A** for blocks A–E (they draw
under 300 mA together) — that is the real protection. Breadboard tie strips
carry about 1 A; a 2 A fuse takes seconds to open at 5 A, by which time the
contacts have melted. If you are on an SMPS brick with no current limit, add a
2 A fuse as a backstop, but the limit is what saves the board.

### Relay contacts

The breadboard relay's contact side switches **only** the 12 V rail or a
low-voltage lamp. Never a mains contactor coil. The production GEN/MAINS
channels are volt-free pairs on their own terminal block (J15) precisely
because those coils run on a separate AC source — that circuit does not belong
on a bench.

---

## Shared kit (buy once)

| Qty | Item | Why / notes |
|----:|------|-------------|
| 1 | **"Black Board F407VGT6" core board** | Replaces U12 + Y1 + Y2 + VCAP caps + VDDA filter + 3.3 V LDO. Same MCU as the design, so the firmware pin map ports directly. |
| — | *Not* the STM32F4-Discovery | Its on-board peripherals sit on the pins this design needs: PA4–PA7 (audio codec + accelerometer) are the sender/battery/D+ ADC inputs, PB6 (codec I2C with a pull-up) is the MPU capture, PD4/PD5 are relay outputs, and PA9/PA10 are USB VBUS/ID — so USART1 debug does not work without desoldering. |
| 1 | ST-Link V2 (clone is fine) | SWD flash + debug |
| 1 | USB-UART adapter, 3.3 V (CP2102 / CH340) | Debug UART — PA9 TX / PA10 RX |
| 1 | 12 V 2 A bench supply with adjustable current limit | The genset rail — the real set is 12 V / 75 Ah. The limit is the safety feature |
| 1 | **12 V → 5 V buck module** (LM2596 or MP1584) | Stand-in for U1/L2/C5/C6. Do **not** breadboard the LM5164 — a 100 V 300 kHz switcher needs the tight PCB loop that block exists to provide |
| 2–3 | Breadboards (830-point) + jumper wire kit | One per block keeps working blocks intact |
| 1 | **Soldering iron, solder, hookup wire** | Required: the relay (block B) and the CT burden (block F) must be soldered, not plugged |
| 1 | Multimeter | Mandatory |
| 1 | Oscilloscope (20 MHz analog or a cheap USB scope) | Needed for blocks D, E, F, G |
| 1 | Function generator (or a scope with one) | Simulates the MPU pickup in block E |
| 20 | **100 nF ceramic** | Decoupling — one per IC supply pin, see below |
| 4 | **10 µF electrolytic** | Rail bulk (and C91 in block D) |
| — | 0.25 W resistor assortment, 1 % where noted | Everything is 1/4 W unless stated |
| — | Film (MKT) capacitor assortment | For the 470 nF and 100 nF signal filters |

### Rail and decoupling convention — applies to every block

- 12 V, 5 V and 3.3 V (from the dev board) with a **single common ground**
  shared by the dev board and the 12 V supply. A missing ground bond between
  two supplies is the most common first-time analog fault.
- **100 nF from every IC's supply pin to ground, within one row of the pin**,
  plus 10 µF bulk per rail per breadboard. The production board carries C63–C67
  for exactly this. An undecoupled LM358 on 12 V, or an MCP6002 driving 10 µF,
  fed from a switching buck module, *will* oscillate — and you will blame the
  topology.

---

## Block A — Digital input channel (1 of 8)

Reference: [04-digital-input.png](../circuits/04-digital-input.png)

**Nodes:** `+12 V → switch → R201 → node`; `node → R211 → GND`;
`node → C201 → GND`; `node → R221 → MCU pin`; clamp diodes at the MCU pin.

| Qty | Prototype part | Production | Note |
|----:|----------------|-----------|------|
| 1 | 5.6 kΩ 1/2 W | R201 | Top of divider — the 1/2 W rating is real, it eats the surge energy |
| 1 | 1.8 kΩ | R211 | Bottom of divider → 2.92 V at the node, 1.6 mA wetting current |
| 1 | 1 µF film | C201 | Debounce on the divider node, τ ≈ 1.4 ms |
| 1 | 10 kΩ | R221 | Node → clamp and MCU pin. This is the resistor that limits fault current into the pin |
| 2 | 1N4148 | D10 (BAT54S) | One anode→pin, cathode→3.3 V; one anode→GND, cathode→pin. Leakage (25 nA) is actually *lower* than the BAT54S it replaces |
| 1 | 5.6 kΩ (optional) | R231 + JP4 | Pull-up from the input terminal to +12 V, for **ground-switched contacts** — many panel switches close to ground rather than feeding 12 V in. Fit the pull-up **or** the field 12 V feed, never both |
| 1 | Toggle switch or jumper | Field contact | |

**Test:** 2.92 V at the node with the switch closed, 0 V open; clamped pin sits
near 4.0 V with ~0.2 mA injection, both inside the STM32's limits. Then read it
in firmware and confirm the debounce rejects a deliberately bouncy contact.
Build **one** channel — the other seven are identical.

---

## Block B — Relay driver (1 of 6)

Reference: [09-relay-driver.png](../circuits/09-relay-driver.png)

**G5LE-1 pin map** (verified against the footprint and the Omron drawing —
guessing this wrong is the single most expensive mistake in the whole design,
and it already happened once during the PCB review):

| Pin | Function |
|-----|----------|
| 2, 5 | **Coil** (pin 5 → +12 V, pin 2 → driver collector/drain) |
| 1 | COM |
| 3 | NO |
| 4 | NC (unused) |

**Nodes:** `MCU pin → R60 → base`; `base → R61 → GND`; `emitter → GND`;
`collector → relay pin 2`; `relay pin 5 → +12 V`; `flyback cathode → +12 V,
anode → drain`; `relay pin 1 (COM) → +12 V`; `pin 3 (NO) → load`.

| Qty | Prototype part | Production | Note |
|----:|----------------|-----------|------|
| 1 | **G5LE-1-DC12** | K1 | Identical part. Solder short wires to its pins rather than forcing it into the breadboard |
| 1 | **BC337-40** (TO-92) | Q60 (2N7002K) | **Preferred.** The obvious substitute, 2N7000, is *not* logic-level: its V_GS(th) spans 0.8–3.0 V and it is characterised at 10 V gate drive, so a worst-case part barely turns on from 3.3 V. If you do use a 2N7000, measure V_GS(th) first and sort the batch. **Pinout warning: the BC337 is E-B-C from the flat face — the opposite order to the BC557B in block C.** Check both with a DMM before powering |
| 1 | 1N5819 (DO-41) | D60 (SS34) | Flyback, **cathode to +12 V** |
| 1 | **1 kΩ** | R60 (100 Ω) | Base resistor. The relay coil is 360 Ω / 33.3 mA, so at hFE ≥ 100 the base needs 0.33 mA; 3.3 V through 1 kΩ gives 2.6 mA — 15× overdrive, hard saturation. The production 100 Ω is a MOSFET gate-stopper and has no function on a BJT: fit the 1 kΩ instead, not both |
| 1 | 100 kΩ | R61 | Base pull-down — keeps the relay off while the MCU is in reset |
| 1 | LED, 5 mm, any colour | — | Optional indicator, **from NO (pin 3) to GND, with COM (pin 1) tied to +12 V** — in series with the 3.3 kΩ below. In parallel with the contacts it would light when the relay is *open* |
| 1 | 3.3 kΩ | — | Series resistor for that LED: 6.7 mA, 0.15 W. A 1 kΩ would dissipate 0.48 W and cook a 1/4 W part |

**Test:** drive from an MCU pin, hear it click, and scope the collector on release —
it should not ring far above 25 V. With the supply current-limited, pulling the
flyback diode for one release is a safe and memorable demonstration of what it
prevents.

---

## Block C — Sender input, current source (1 of 3)

Reference: [05-sender-input.png](../circuits/05-sender-input.png)

The only block with a real op-amp loop, and the one most worth building.

**Nodes:** `+5 V → R20 → emitter`; `emitter → op-amp − input` (feedback);
`4.5 V reference (R71/R70 from +5 V, C60 to GND) → op-amp + input`;
`op-amp output → R18 → base`; `collector → sender → GND`;
`collector → R23 → C20 → R26 → ADC pin`; clamp at the ADC pin.

| Qty | Prototype part | Production | Note |
|----:|----------------|-----------|------|
| 1 | **LM358N** (DIP-8) | U3 | Runs from **+12 V** — its input common-mode range (V+ − 1.5 V) must clear the 4.5 V reference, which it does |
| 1 | **BC557B** (TO-92) | Q3 (BC857) | PNP pass device. Pinout from the flat face, legs down, is **C-B-E** (left pin = collector). Confirm with a DMM diode test before powering: the middle pin should read as the base to both outers |
| 1 | 62 Ω 1 % | R20 | Sets 8.06 mA for the oil and fuel channels |
| 1 | 249 Ω 1 % | R22 | Alternative to R20 for the **temperature** channel: 2.0 mA, which avoids saturating a cold NTC. Build one channel or the other |
| 1 | 90.9 kΩ 1 % | R71 | With R70, sets the 4.5 V reference off +5 V |
| 1 | 10 kΩ 1 % | R70 | |
| 1 | 100 nF | C60 | Reference decoupling |
| 1 | **1 kΩ** | **R18** | Op-amp output → base. Limits the loop on an open sender, where the op-amp otherwise drives the base at its ~40 mA short-circuit current |
| 1 | 1 kΩ | R23 | Series into the filter |
| 1 | 100 nF | C20 | ADC filter |
| 1 | **4.7 kΩ** | **R26** | Filter → clamp and ADC pin. **This is the resistor that makes the fault survival true**: with the sender shorted to +12 V it holds clamp current to ~3.6 mA, against the STM32's ±5 mA injection limit. Without it the fault drives 20 mA into the pin |
| 2 | 1N4148 | D20 (BAV199) | Clamp to 3.3 V / GND |
| 1 | **200 Ω pot** or resistance decade box | Sender | Stands in for the oil sender, 0–184 Ω |

**Test:** sweep the pot and confirm the voltage across it tracks
`V = 8.06 mA × R` — 184 Ω should read ≈1.48 V. That linearity is the circuit's
whole purpose; a resistor divider cannot do it. Then read it with the ADC and
check the firmware's resistance→pressure lookup.

---

## Block D — VREF_MID 1.65 V buffer (needed by blocks F and G)

Reference: [13-vref-mid-buffer.png](../circuits/13-vref-mid-buffer.png)

**Nodes:** `3.3 V → R90 → mid`; `mid → R91 → GND`; `mid → C90 → GND`;
`mid → op-amp + input`; `op-amp output → R92 → VREF_MID rail`;
`VREF_MID rail → C91 → GND`; `VREF_MID rail → R93 → op-amp − input`;
`op-amp output → C93 → op-amp − input`.

| Qty | Prototype part | Production | Note |
|----:|----------------|-----------|------|
| 1 | **MCP6002-I/P** (DIP-8) | U8 | Rail-to-rail, from 3.3 V. Second half is spare |
| 2 | 10 kΩ 1 % | R90, R91 | Divider → 1.65 V |
| 1 | 100 nF | C90 | At the divider node |
| 1 | 47 Ω | R92 | Output isolation |
| 1 | 10 µF | C91 | Bulk on the buffered rail |
| 1 | 10 kΩ | R93 | **DC** feedback, from **after** the 47 Ω |
| 1 | 100 nF | C93 | **AC** feedback, from the op-amp output **directly** |

**Test:** the output should read half of whatever your 3.3 V rail actually
measures, within about 1 % — the absolute value does not matter, because the
ADC is ratiometric to the same reference. What matters is that it is *stable*:
scope it while loading it and confirm no oscillation. Wiring both feedback
components to the same point is the classic way to make this circuit ring, and
is worth trying once deliberately to see the failure.

---

## Block E — MPU / RPM comparator

Reference: [06-mpu-rpm-input.png](../circuits/06-mpu-rpm-input.png)

**Supply is split and it matters:** the LM393 runs from **+5 V**, and its
open-collector output is pulled up to **+3.3 V** by R35. That is what level-shifts
it safely into the MCU. Do not power it from 3.3 V — the LM393's input
common-mode limit is V+ − 1.5 V, which at 3.3 V is 1.8 V, leaving no room above
the 1.65 V threshold.

**Nodes:** `pickup → C30 → R30 → + input`; `+ input → R31 → 3.3 V`;
`+ input → R32 → GND`; `− input → R33 → 3.3 V`; `− input → R34 → GND`;
`output → R36 → + input`; `output → R35 → 3.3 V`; `output → MCU timer pin`.

| Qty | Prototype part | Production | Note |
|----:|----------------|-----------|------|
| 1 | **LM393N** (DIP-8) | U5 (LM2903) | Same part, leaded. **V+ = +5 V** |
| 1 | 100 nF film, 100 V | C30 | AC-couples the pickup |
| 2 | **100 kΩ** | R31, R32 | Bias the + input to 1.65 V. Their parallel 50 kΩ sets the hysteresis with R36 |
| 2 | 10 kΩ | R33, R34 | Threshold on the − input, also 1.65 V |
| 1 | 1 MΩ | R36 | Positive feedback → ±79 mV (3.3 V × 50 k / 1.05 M gives a 157 mV window). Leave it out once to watch the output chatter |
| 1 | 10 kΩ | R35 | Pull-up **to +3.3 V** — the open-collector output is the level shifter |
| 1 | 10 kΩ | R30 | Input series. With the bias network it attenuates by 0.83, which sets the sensitivity floor |
| 2 | 1N4148 | D30 (BAV99) | Clamp — a real pickup swings ±50 V at speed |
| 1 | Function generator | MPU pickup | 100 Hz–5 kHz sine |

**Test:** feed a sine at **≥400 mVpp** and expect a clean square wave at the
same frequency. The real sensitivity floor is ≈190 mVpp at the connector
(R30's attenuation against the 157 mV hysteresis window), so testing at 200 mVpp
sits right on the edge and will chatter — that is the circuit working, not a
fault. 1500 rpm on a 118-tooth ring gear is 2950 Hz; check the firmware's
computed rpm against the generator.

---

## Block F — CT current input

Reference: [08-ct-input.png](../circuits/08-ct-input.png)

**Wiring, and this part is not optional:** solder the burden resistor and the
clamp **directly across the CT's own leads**, with short wire. Take only two
high-impedance sense wires from the burden's ends into the op-amp. The CT's
cold end goes to VREF_MID *at the burden*, not through the breadboard.

Two reasons. First, a breadboard jumper in the CT loop is an open-circuit
waiting to happen. Second, the full secondary current (215 mA in the example
below, 5 A at CT rating) would otherwise flow through breadboard contacts:
~30 mΩ of contact resistance injects ~6.5 mV in series with a 10.75 mV signal —
a 60 % error you would trust — and it corrupts VREF_MID for block G too. On the
PCB this is a Kelvin-routed star for exactly this reason.

**Nodes:** `CT+ → burden → CT−`; `CT− → VREF_MID` (at the burden);
`CT+ → op-amp + input`; `op-amp − input → R51 → VREF_MID`;
`op-amp − input → R52 → op-amp output`; `output → R53 → C50 → ADC pin`.

| Qty | Prototype part | Production | Note |
|----:|----------------|-----------|------|
| 1 | **MCP6002-I/P** (DIP-8) | U9 | Gain = 1 + R52/R51 = ×2 about the 1.65 V bias |
| 1 | 0.05 Ω 3 W wirewound (or 2× 0.1 Ω 2 W in parallel) | R50 | Burden — **soldered to the CT leads** |
| 2 | 10 kΩ 1 % | R51, R52 | Gain pair. 1 % matters: two 5 % parts give up to 7 % gain error on every current and power reading |
| 1 | 6.8 kΩ 1 % | R53 | Phase match to the voltage channel |
| 1 | 22 nF C0G | C50 | Anti-alias |
| 1 | 5.6 V bidirectional TVS (or 2× 5.1 V zeners back-to-back) | D50 | Across the CT terminals, soldered with the burden |
| 1 | **Split-core CT, 100 A : 5 A** | Field CT | |

**Test:** run a known load and pass the conductor through the split core
**5 times**, so a 1 kW heater (4.3 A at 230 V) presents 21.5 A — about 21 % of
the CT's rating, inside the 5–120 % band where its ratio and phase error are
actually specified. At 4.3 A single-pass you are at 4 % of rating and the CT
itself contributes several percent of error. Divide the reading by 5.

Single-pass arithmetic for reference: 4.3 A ÷ 20 = 215 mA secondary × 0.05 Ω =
10.75 mV RMS, ×2 gain ≈ 21.5 mV about 1.65 V. Small — which is why the gain
stage exists and why the ADC needs a stable 1.65 V from block D. Only the CT
clamps around the mains conductor; nothing on the breadboard touches line
voltage.

---

## Block G — AC voltage sense, low-voltage stand-in only

Reference: [07-ac-voltage-sense.png](../circuits/07-ac-voltage-sense.png)

Read the safety section first. Build this against a **12 V AC wall
transformer**, never line voltage.

**Nodes:** `transformer leg A → R_top → node`; `node → R_bot → VREF_MID`
(not GND — that is what centres the AC swing in the ADC range);
`node → R305 → C40 → GND`; `node → clamp`; `transformer leg B → GND`.

| Qty | Prototype part | Production | Note |
|----:|----------------|-----------|------|
| 1 | 12 V AC transformer, plug-in, 500 mA | 415 V line | Isolated secondary, safe to probe |
| 1 | **62 kΩ** | R300–R303 (4× 330 k) | See the sizing note below |
| 1 | **5.6 kΩ** | R304 (5.62 k) | Keep the production bottom leg |
| 1 | 1 kΩ | R305 | Series into the ADC |
| 1 | 22 nF C0G | C40 | Anti-alias |
| 2 | 1N4148 | D40 (BAV199) | Clamp to 3.3 V / GND |
| — | Bias from **block D** | VREF_MID | |

**Sizing — measure before you build.** A small unregulated plug transformer
runs 15–25 % high off-load, and this divider draws essentially nothing, so a
"12 V" unit typically reads 14–15 V RMS open-circuit. Measure yours first. The
62 k / 5.6 k pair gives ÷12.07, which puts 12 V RMS at 1.41 Vpk — matching what
the production chain produces from 340 Vpk (÷235.9 → 1.44 Vpk). If your
transformer reads 15 V RMS, size the top leg for ~1.4 Vpk from *that* instead,
and confirm on the scope that the peaks clear 0 V and 3.3 V before trusting any
firmware number — a clipped waveform gives a plausible-looking wrong RMS.

**Why 62 k / 5.6 k and not 100 k / 9.1 k.** Both divide correctly, but the
divider's source impedance sets the filter corner, and the design deliberately
phase-matches this channel to the CT channel within 0.1°. Production: 5.62 k ∥
1320 k + 1 k with 22 nF → 1097 Hz, −2.61° at 50 Hz, against the CT channel's
−2.69°. Keeping the 5.6 k bottom leg gives 6.14 k source → 1179 Hz, −2.43° —
within 0.27°. A 100 k / 9.1 k pair would give 775 Hz and −3.70°, a full degree
of extra lag, which is ~1.3 % power error and ~0.011 PF error at PF 0.8: a
wrong number you would trust.

**Test:** expect a 1.41 Vpk sine on 1.65 V (≈0.12 V to 3.06 V). Run the
firmware's RMS and frequency measurement and compare against a meter on the
transformer output. Moving to the real board changes only the divider ratio —
the maths, the filter and the code are already proven.

---

## Block H — Comms (optional, module-based)

References: [10-can-transceiver.png](../circuits/10-can-transceiver.png),
[11-rs485-transceiver.png](../circuits/11-rs485-transceiver.png),
[17-eeprom-spi.png](../circuits/17-eeprom-spi.png)

| Qty | Prototype part | Production | Note |
|----:|----------------|-----------|------|
| 1 | **SN65HVD230 CAN module** | U6 (TJA1051T/3) | 3.3 V. Most breakout modules have a **fixed** 120 Ω that has to be desoldered, not a jumper — check before using two on one bus |
| 1 | **MAX3485 module** (3.3 V) | U7 (THVD1450) | Prefer the 3.3 V MAX3485 over the 5 V MAX485 — the MAX485's RO output is 5 V logic (survivable on PD9, which is 5 V-tolerant, but not on every pin). Neither module has the THVD1450's true fail-safe receiver, so fit the 560 Ω bias resistors that the real board carries as belt-and-braces |
| 1 | **25LC1024-I/P** (DIP-8) | U11 (M95M02-DR) | Use this, **not** the 25LC256: the M95M02 takes **3** address bytes and a 256-byte page, the 25LC256 takes 2 and 64. A driver validated on a 25LC256 addresses the real part wrongly on the first byte and mis-pages every write. The 25LC1024 matches on both counts |
| 3 | 10 kΩ | R95, R96, R97 | Pull-ups on CS, WP and HOLD — floating WP/HOLD on a 25LCxxx is unreliable |
| 2 | 560 Ω | R73, R74 | RS485 fail-safe bias. The modules lack the THVD1450's true fail-safe receiver, so unlike the real board these are **not** belt-and-braces — without them the idle bus state is undefined |
| 1 | USB-CAN adapter (CANable / SocketCAN) | — | Replay J1939 frames from the PC against the firmware — the fastest way to test the J1939 module without an engine |

---

## Suggested build order

1. **A** (digital input) — smallest, proves rails and firmware I/O
2. **B** (relay driver) — an input can now switch an output
3. **D** (VREF buffer) — everything analog needs it; build it early and leave it
4. **C** (sender) — the first real analog measurement
5. **E** (MPU) — rpm, which the engine FSM depends on
6. **F** (CT) — needs D and a known load
7. **G** (AC sense, low voltage) — needs D; last, because it needs the most care
8. **H** (comms) — any time, independent of the analog blocks

Blocks A–E together are enough to run the engine FSM end to end: a start
request, crank, rpm rising past the disconnect threshold, oil pressure
building, and shutdowns on the protections you deliberately trip.

## Regenerating the production BOM

```
kicad-cli sch export netlist hardware/kicad/ecu25-main/ecu25-main.kicad_sch -o /tmp/ecu25.net
python3 hardware/kicad/gen/gen_bom.py /tmp/ecu25.net
```
