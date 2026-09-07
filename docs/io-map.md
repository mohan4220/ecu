# ECU-25 — Signal Map

**Every quantity the controller measures, every output it drives, and exactly what it commands on the engine ECU.**

Target set: Kirloskar KG4-25WS1 — 25 kVA, 415 V, 50 Hz, engine 3R550ETA 4G1 (3-cylinder 1.65 L CRDi, CPCB IV+, EGR + DOC), 12 V / 75 Ah electrical system.

Derived from the schematic generator (`hardware/kicad/gen/gen_ecu25.py`), the firmware types (`firmware/core/gcu_types.h`), and the two field protocols. Component values trace to the reviewed calculation sheet, [docs/circuits/README.md](circuits/README.md).

| Group | Count | Detail |
|---|---|---|
| **Analog measurement channels** | 14 | 6 AC voltage, 3 AC current, 3 engine senders, battery, D+ |
| **Frequency input** | 1 | magnetic pickup |
| **Digital inputs** | 8 | 5 assigned, 3 spare |
| **Engine data over J1939** | 5 PGNs | rpm, oil, coolant, fuel, DM1 lamps + DTC |
| **Operator keys** | 3 | start, stop, mode |
| **Relay outputs** | 6 | all dry contacts |
| **Other outputs** | 8 | D+ excitation, backlight, SPI selects, RS485 DE, LED |
| **Alarms evaluated** | 21 | every 10 ms tick |

**Two things this map makes explicit**, because they are the most common misunderstanding about this class of controller:

1. **Most engine data has two possible sources.** On this engine, rpm / oil pressure / coolant temperature / fuel level arrive over **J1939** from the engine's own ECU. The analog sender front end is the **legacy path**, kept for mechanically-governed engines and as a cross-check. The control logic never knows which source filled the value: the runtime **merges** them, so a fresh J1939 value wins and a stale one leaves the sender reading standing. An unconditional overwrite would wipe four good readings the moment the bus went quiet — and permanently on a legacy engine with no ECU.
2. **ECU-25 does not run the engine. It supervises it.** Over CAN it is a listener only: it transmits nothing but its own address claim. Its entire physical authority over the engine is one relay contact — run-enable — plus a starter pilot. See §8.

---

## 1. AC electrical measurement

Six voltage channels and three current channels, all sampled together against the buffered VREF_MID 1.65 V bias, so the ADC sees a signed waveform on a single supply.

| # | Quantity | Sensor / source | Terminal | Measuring range | Front end | Scaling | MCU pin |
|---|---|---|---|---|---|---|---|
| 1 | Generator L1–N volts | direct connection to alternator terminals | J5-1 | 0–276 V RMS (clips), nominal 240 | 4 × 330k + 5.62k divider, biased to 1.65 V, 1.1 kHz filter | ÷235.9 | `PC0` |
| 2 | Generator L2–N volts | " | J5-2 | " | " | ÷235.9 | `PC1` |
| 3 | Generator L3–N volts | " | J5-3 | " | " | ÷235.9 | `PC2` |
| 4 | Mains L1–N volts | direct connection to the utility side | J6-1 | " | " | ÷235.9 | `PC3` |
| 5 | Mains L2–N volts | " | J6-2 | " | " | ÷235.9 | `PC4` |
| 6 | Mains L3–N volts | " | J6-3 | " | " | ÷235.9 | `PC5` |
| 7 | Load current L1 | **current transformer** on the phase cable | J7-1 / J7-2 | 0–117 A primary with 50:5 | 0.05 Ω burden → MCP6002 × 2, phase-matched filter | 1 A sec → 0.1 V | `PA0` |
| 8 | Load current L2 | " | J7-3 / J7-4 | " | " | " | `PA1` |
| 9 | Load current L3 | " | J7-5 / J7-6 | " | " | " | `PA2` |

Neutral lands on J5-4 / J6-4. Voltages are measured **line-to-neutral**; 415 V line-to-line appears between adjacent phases, which is why J5/J6/J15 are 8-pole bodies with only the odd poles wired (10.16 mm live-to-live pitch).

**Frequency is not a separate channel.** Generator and mains Hz are derived from the L1 voltage waveforms in `ac_sense.c`, by interpolated zero crossings.

> **CT sizing — confirm before you buy.** The schematic and firmware now both specify **50:5**, in one place each (`J7` text, and `ECU25_CT_PRIMARY_A` in `firmware/core/ecu_main.h`). Fitting a different ratio without changing that constant scales every current reading by the error. A 25 kVA / 415 V set draws 34.8 A at full load, which is 0.87 A on the secondary — about 17 % of the channel's designed swing, so most of the ADC range goes unused and light-load current is coarse. **50:5** puts full load at ~69 % of span and still does not clip until ~117 A primary (3.4 × overload). Nothing on the board changes; only the part you order. The table above assumes 50:5.

## 2. Engine sensors — the analog (legacy) path

Three constant-current sender inputs. A current source, not a divider, so the reading does not shift with supply voltage or wiring resistance. Sender types below are the common ones — **calibrate against whatever is actually fitted to your engine.**

| # | Quantity | Sensor | Terminal | Range | Excitation | Full scale | MCU pin |
|---|---|---|---|---|---|---|---|
| 10 | Oil pressure | resistive pressure sender (VDO / Datcon type, typically 10–184 Ω) | J3-1 | 0–10 bar | 8.06 mA (0.5 V / 62 Ω) | 184 Ω → 1.48 V, ≈0.1 Ω resolution | `PA3` |
| 11 | Fuel level | resistive float sender in the tank | J3-2 | 0–100 % | 8.06 mA | sender-dependent | `PA4` |
| 12 | Coolant temperature | NTC thermistor sender | J3-3 | −40 to +150 °C | 2.0 mA (0.5 V / 249 Ω) | lower current avoids saturating a cold NTC | `PA5` |

J3-4 is the sender return. Each channel has an RC filter plus a 4.7 k series resistor and a BAV199 clamp, holding fault current into the MCU pin to ≈3.6 mA if a sender shorts to +12 V.

| # | Quantity | Sensor | Terminal | Range | Front end | MCU pin |
|---|---|---|---|---|---|---|
| 13 | Engine speed | **magnetic pickup** on the flywheel ring gear | J4-1 / J4-2 | 0–4000 rpm | AC-coupled → LM2903 comparator, ±80 mV hysteresis, BAV99 clamps ±50 V pickup swings | `PB6` (TIM4_CH1) |

## 3. DC electrical measurement

| # | Quantity | Sensor / source | Terminal | Range | Front end | Scaling | MCU pin |
|---|---|---|---|---|---|---|---|
| 14 | Battery voltage | direct, at the board's own supply terminal | J1-1 | 0–18.3 V | 100k / 22k divider, BAV199 clamp | ÷5.545, 4.5 mV per count | `PA6` |
| 15 | Charge alternator D+ | alternator D+ / L terminal | J13 | 0–18.3 V | 120 Ω 3 W excitation + 100k / 22k sense | ÷5.545 | `PA7` |

**D+ is both an input and an output.** The 120 Ω resistor feeds the ~100 mA the alternator's field needs to start self-exciting; the same node is then measured to detect charge failure (D+ below `charge_fail_ratio` × battery volts for `charge_fail_delay_ms`).

## 4. Digital inputs

Eight identical channels on J2 (plus two GND poles). Divider 5.6k / 1.8k from 12 V, BAT54S clamp to the rails, 1 µF debounce cap (τ ≈ 1.4 ms), plus a firmware 3-sample debounce. Wetting current ≈1.6 mA — enough to break oxide on a dirty panel contact. Each channel has a solder jumper (JP4–JP11) that fits a 1 k pull-up instead, so the channel accepts a **ground-switched** contact. Fit the pull-up *or* the field 12 V feed, never both.

| # | Ch | MCU pin | Firmware field | Sensor / switch | Class |
|---|---|---|---|---|---|
| 16 | DIN1 | `PE0` | `emergency_stop` | E-stop mushroom button, **fail-safe** (a broken wire reads as pressed) | shutdown |
| 17 | DIN2 | `PE1` | `remote_start` | External start command — BMS, building controller, dry contact | command |
| 18 | DIN3 | `PE2` | `low_oil_switch` | Engine's own low-oil-pressure switch | shutdown |
| 19 | DIN4 | `PE3` | `high_coolant_switch` | Engine's own high-coolant-temperature switch | shutdown |
| 20 | DIN5 | `PE4` | `low_coolant_level` | Coolant level float switch | warning |
| 21–23 | DIN6–8 | `PE5`–`PE7` | — | **Spare** — wired to the MCU, no function assigned | — |

**The mapping is now fixed**, in exactly one place: the `DIN_*` defines at the top of `firmware/core/ecu_main.c`. Change it there and change this table; nothing else encodes it. Label the panel to match.

## 5. Engine data over J1939

Listener only. Every value is aged; once its age passes its timeout the value is marked **invalid** rather than going stale, and the controller falls back or alarms.

| PGN | Name | SPN | Quantity | Resolution | Broadcast rate | Timeout |
|---|---|---|---|---|---|---|
| 61444 | EEC1 | 190 | Engine speed | 0.125 rpm/bit | 10–20 ms | 500 ms |
| 65263 | EFL/P1 | 100 | Oil pressure | 4 kPa/bit | 500 ms | 2 s |
| 65262 | ET1 | 110 | Coolant temperature | 1 °C/bit, −40 offset | 1 s | 3 s |
| 65276 | DD | 96 | Fuel level | 0.4 %/bit | 1 s | 5 s |
| 65226 | DM1 | — | Red (stop) lamp, amber (warning) lamp, first DTC as SPN + FMI | — | 1 s | 3 s |
| 60928 | AC | — | Address claim — contention resolved by lower NAME | — | — | — |
| 59904 | RQST | — | Request; answered for the address-claim PGN | — | — | — |

Single-frame DM1 only: TP.BAM multi-DTC transfers are not reassembled, but the lamp bits still carry the severity, which is what drives the shutdown. **Comms loss** raises only after run-enable has been on past a 2 s boot grace — with K1 open the engine ECU is asleep and silence is normal.

## 6. Operator inputs

From the display board over the shared SPI bus (74HC165 key shift register): `key_start`, `key_stop` (which also resets latched alarms), and `mode_auto`.

## 7. Derived values and alarms

| Value | Computed from | Module |
|---|---|---|
| RMS volts and amps, all 9 channels | √(mean(x²) − mean(x)²) over a 100 ms window; DC bias measured, never assumed | `ac_sense.c` |
| Generator / mains frequency | interpolated rising zero crossings on the L1 waveform | `ac_sense.c` |
| Total real power (signed) | mean(v·i) − mean(v)·mean(i), per phase, summed | `ac_sense.c` |
| Power factor | \|P\| / Σ(V_rms × I_rms) | `ac_sense.c` |
| Run hours | ticks with the engine actually turning (rpm > 100), not ticks with K1 closed | runtime |
| Engine state, AMF state | state machines | `engine_fsm.c`, `amf_fsm.c` |
| 21 alarm conditions | thresholds + qualification delays | `protection.c` |

**Alarms** (each classified warning or shutdown, most with a configurable threshold and delay): `EMERGENCY_STOP`, `LOW_OIL_PRESSURE`, `HIGH_COOLANT_TEMP`, `OVERSPEED`, `UNDERSPEED`, `GEN_UNDER_VOLT`, `GEN_OVER_VOLT`, `GEN_UNDER_FREQ`, `GEN_OVER_FREQ`, `OVERCURRENT`, `CHARGE_FAIL`, `BATT_LOW`, `BATT_HIGH`, `LOW_COOLANT_LEVEL`, `FAIL_TO_START`, `FAIL_TO_STOP`, `SENSOR_LOSS`, `ECU_RED_LAMP`, `ECU_WARNING`, `ECU_COMMS_LOST`.

---

## 8. What the controller drives

### 8.1 Relay outputs

Six G5LE-1-DC12 relays, low-side driven by 2N7002K FETs with SS34 flyback diodes. Coils run from +12V_SW (33.3 mA each, 200 mA total). **Every contact is dry and pilot-duty — no field current crosses the PCB.**

| Relay | MCU pin | Net | Contact terminal | Drives |
|---|---|---|---|---|
| K1 FUEL | `PD2` | `RLY_FUEL` | J8-2 | Engine ECU **run enable** (CRDi) or the fuel solenoid (legacy engine) |
| K2 START | `PD3` | `RLY_START` | J8-3 | **Pilot only** — the coil of an external starter relay |
| K3 GEN | `PD4` | `RLY_GEN` | J15 volt-free pair | Generator contactor coil |
| K4 MAINS | `PD5` | `RLY_MAINS` | J15 volt-free pair | Mains contactor coil |
| K5 AUX1 | `PD6` | `RLY_AUX1` | J8-4 | Configurable — default **horn** |
| K6 AUX2 | `PD7` | `RLY_AUX2` | J8-5 | Configurable — default **preheat** |

**J8 pins 1 and 6 are both OUT_COM** — the common contact supply, which the *installer* feeds from their own fused source. K1/K2/K5/K6 contact commons come from that, not from the board rail. **Ceiling: 4 A total, 2 A per output, installer fuse 4 A.**

**START must never drive a starter motor solenoid directly.** A 12 V solenoid pulls 20–40 A and glow plugs 28–60 A, against 8 A DC contacts. The contacts would weld — and a welded starter contact means an engine that cranks and cannot be commanded to stop.

**K3/K4 are volt-free pairs (COM + NO) on their own block J15**, because contactor coils run from their own AC source and the panel's hardware interlock must stay load-bearing.

K5/K6 select from `gcu_aux_fn_t`: `AUX_OFF`, `AUX_HORN`, `AUX_PREHEAT`, `AUX_RUNNING`, `AUX_FAULT`.

### 8.2 Other outputs

| Output | MCU pin | Purpose |
|---|---|---|
| D+ excitation | — (analog) | ~100 mA into the charge alternator field via 120 Ω 3 W |
| Display backlight PWM | `PE11` (TIM1_CH2) | Dimming; also the crank-time load shed |
| TFT reset | `PE8` | RA8875 display controller reset |
| TFT / LED / key chip selects | `PB7` / `PB8` / `PB9` | Shared SPI1 bus to the display board |
| LED shift register | via `PB8` | Panel indicator LEDs (74HC595) |
| EEPROM chip select | `PB12` | M95M02 config + run hours (SPI2) |
| RS485 direction | `PD10` | DE for the Modbus half-duplex turnaround |
| Heartbeat LED | `PA8` | Board-alive indicator |

### 8.3 Remote control surface — Modbus RTU

RS485 on J10. Four holding registers, deliberately small; 23 input registers publish the live measurement snapshot read-only.

| Reg | Function |
|---|---|
| 0 | Mode: 0 off, 1 manual, 2 auto, 3 test |
| 1 | Remote start request (0/1) |
| 2 | Alarm reset — write 1 to pulse, always reads back 0 |
| 3 | Lamp test (0/1) |

---

## 9. What we control on the engine ECU

This is the question worth being precise about, because a common-rail engine ECU manages its own injection, timing, rail pressure, EGR and cold-start aid. **ECU-25 does not touch any of it.**

**Physical authority — two contacts:**

- **Run enable (K1).** The single most important output. Closed, the engine ECU is permitted to run; open, it stops. On this engine that is an ECU enable input; on a legacy mechanical engine the same contact drives the fuel solenoid. K1 energises from the PREHEAT state onward, not just at crank, to give the ECU its boot time before the starter engages.
- **Starter pilot (K2).** Energises an external starter relay. Crank-disconnect is decided here — on rpm crossing `crank_disconnect_rpm` or oil pressure crossing `crank_disconnect_oil_bar`, whichever comes first.

**Over CAN — nothing.** ECU-25 transmits only its own J1939 address claim. No TSC1 torque/speed command, no shutdown request, no mode change. Cut the CAN cable and the engine keeps running exactly as before; only the data goes away, and `ECU_COMMS_LOST` raises.

**What the engine ECU owns, and we deliberately do not:** injection quantity and timing, rail pressure, idle governing, speed droop, EGR, DOC management, glow-plug / cold-start aid (which is why K6 preheat is *configurable* rather than hard-wired), and its own protection shutdowns — those reach us as DM1 lamp bits, and a red lamp makes us open K1.

**Everything else the controller commands is switchgear, not engine control:** the generator contactor, the mains contactor, and the AMF transfer sequence between them (break-before-make, with a dead time, the panel's mechanical interlock still the final authority).

---

## 10. Against a reference AMF controller

The design target is DSE 4520 class — the same functional class as the Kirloskar KG-series controller on the reference sheet. This is a functional checklist of what a controller in that class monitors and switches, and where ECU-25 stands.

*The KG640C sheet itself is not in this document's source material, so the comparison is against the standard signal set for the class, not line-by-line against that drawing. Re-share the sheet for a terminal-by-terminal comparison.*

| Function | Reference class | ECU-25 | Note |
|---|---|---|---|
| 3-phase generator voltage | yes | **yes** | 3 channels, L-N |
| 3-phase mains voltage | yes | **yes** | 3 channels — AMF needs both sides |
| 3-phase load current | yes | **yes** | CT inputs, 3 channels |
| Generator / mains frequency | yes | **yes** | derived from L1 |
| kW, kVA, power factor | yes | **yes** | `ac_sense.c` |
| Engine speed | yes | **yes** | magnetic pickup **and** J1939 |
| Oil pressure | yes | **yes** | sender **and** J1939 |
| Coolant temperature | yes | **yes** | sender **and** J1939 |
| Fuel level | yes | **yes** | sender **and** J1939 |
| Battery voltage | yes | **yes** | 11.0 / 15.5 V thresholds |
| Charge alternator fail | yes | **yes** | D+ excite + sense |
| Configurable digital inputs | yes | **yes** | 8, of which 3 spare |
| Engine ECU J1939 link | yes (CPCB IV+ sets) | **yes** | listener + DM1 lamps and DTC |
| Fuel / start / gen / mains relays | yes | **yes** | all dry contacts |
| Configurable aux relays | yes | **yes** | 2 (K5 / K6) |
| Modbus RTU over RS485 | yes | **yes** | FC 03 / 04 / 06 / 16 |
| Run hours | yes | **partial** | counted, but **not yet persisted** — the M95M02 driver is unwritten, so hours reset on every power cycle |
| Event / alarm log | yes | **not yet** | alarms are live-only; no stored history |
| Earth / neutral fault CT | some | **no** | would need a 4th CT channel |
| Fuel flow / consumption | some | **no** | no flow sensor input |
| Battery charger control | some | **no** | monitoring only |
| Remote annunciator panel | some | **no** | Modbus covers remote status |
| Load sharing / synchronising | no (4520 class) | **no** | single-set controller by design |
| DEF / SCR monitoring | N/A on this engine | **no** | EGR + DOC — a DOC is passive |
| DPF regeneration UI | N/A on this engine | **no** | same reason |

---

## 11. Connector summary

| Ref | Poles | Carries |
|---|---|---|
| J1 | 2 | Battery + / − |
| J2 | 10 | DIN1–8 + 2 × GND |
| J3 | 4 | Oil, fuel, temp senders + return |
| J4 | 2 | Magnetic pickup |
| J5 | 8 (odd wired) | Generator L1 L2 L3 N — **415 V** |
| J6 | 8 (odd wired) | Mains L1 L2 L3 N — **415 V** |
| J7 | 6 | CT1–3, S1 / S2 each |
| J8 | 6 | OUT_COM, FUEL, START, AUX1, AUX2, OUT_COM |
| J9 | 3 | CAN H / L / shield |
| J10 | 3 | RS485 A / B / shield |
| J13 | 2 | Charge alternator D+ |
| J15 | 8 (odd wired) | GEN and MAINS contactor volt-free pairs |
| J12 | 20-way ribbon | Display board — SPI, selects, backlight power |
| J14 | 3 | Debug UART (USART1) |
| J11 | TC2030 | SWD programming / debug |
