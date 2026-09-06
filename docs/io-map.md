# ECU-25 — signal map: everything measured, everything controlled

Complete I/O inventory of the controller: every quantity it senses, where the
value comes from, how it is scaled, which MCU pin carries it, and every output
it drives. Derived from the schematic generator
(`hardware/kicad/gen/gen_ecu25.py`), the firmware types
(`firmware/core/gcu_types.h`) and the two field protocols.

Two things this map makes explicit, because they are the most common
misunderstanding about this class of controller:

1. **Most engine data has two possible sources.** On the target engine
   (Kirloskar 3R550ETA 4G1, CRDi, CPCB IV+) rpm, oil pressure, coolant
   temperature and fuel level arrive over **J1939** from the engine's own ECU.
   The analog sender front-end is the **legacy path**, retained for
   mechanically-governed engines and as a cross-check. The control logic never
   knows which source filled the value — `j1939_fill_inputs()` overwrites the
   same `gcu_inputs_t` the analog path fills.
2. **ECU-25 does not run the engine. It supervises it.** Over CAN it is a
   listener only: it transmits nothing but its own address claim. Its entire
   physical authority over the engine is one relay contact — run-enable — plus
   a starter pilot. See [What we control on the engine](#what-we-control-on-the-engine-ecu).

---

## 1. Measured — AC electrical

| Quantity | Ch | Terminal | Front end | Scaling | MCU pin |
|---|---|---|---|---|---|
| Generator L1-N volts | 1 | J5-1 | 4×330k + 5.62k divider, biased to 1.65 V | 1/235.9, clips ≈276 V RMS | PC0 |
| Generator L2-N volts | 2 | J5-2 | " | " | PC1 |
| Generator L3-N volts | 3 | J5-3 | " | " | PC2 |
| Mains L1-N volts | 1 | J6-1 | " | " | PC3 |
| Mains L2-N volts | 2 | J6-2 | " | " | PC4 |
| Mains L3-N volts | 3 | J6-3 | " | " | PC5 |
| Load current L1 | 1 | J7-1/2 | CT → 0.05 Ω burden → MCP6002 ×2 | 5 A sec = 0.71 Vpk | PA0 |
| Load current L2 | 2 | J7-3/4 | " | " | PA1 |
| Load current L3 | 3 | J7-5/6 | " | " | PA2 |

Neutral lands on J5-4 / J6-4. Both voltage sets are measured **line-to-neutral**
and all six channels share the VREF_MID 1.65 V bias, so the sampler sees a
signed waveform on a single-supply ADC.

**Frequency** is not a separate channel — generator and mains Hz are derived
from the L1 voltage waveforms in firmware.

> **CT sizing worth revisiting.** The schematic specifies 200:5 CTs. A 25 kVA /
> 415 V set draws 34.8 A at full load, which is 0.87 A on the secondary — only
> ~17 % of the channel's designed swing, so most of the ADC range goes unused
> and low-load current readings are coarse. A **50:5** CT would put full load at
> ~69 % of span and still not clip until ~117 A primary (3.4× overload). Worth
> changing before you buy CTs; nothing on the board changes, only the part.

## 2. Measured — engine sensors (analog path)

Three constant-current sender inputs. A current source, not a divider, so the
reading does not shift with supply voltage or wiring resistance.

| Quantity | Terminal | Excitation | Full scale | MCU pin |
|---|---|---|---|---|
| Oil pressure | J3-1 | 8.06 mA (0.5 V / 62 Ω) | 184 Ω → 1.48 V, ≈0.1 Ω resolution | PA3 |
| Fuel level | J3-2 | 8.06 mA | resistive float sender | PA4 |
| Coolant temperature | J3-3 | 2.0 mA (0.5 V / 249 Ω) | lower current avoids saturating a cold NTC | PA5 |

J3-4 is the sender return. Each channel has an RC filter plus a 4.7 k series
resistor and BAV199 clamp, holding fault current into the MCU pin to ≈3.6 mA if
a sender shorts to +12 V.

| Quantity | Terminal | Front end | Scaling | MCU pin |
|---|---|---|---|---|
| Engine speed (magnetic pickup) | J4-1/2 | AC-coupled → LM2903 comparator, ±80 mV hysteresis, BAV99 clamp for ±50 V swings | pulse train, timed by TIM4_CH1 | PB6 |

## 3. Measured — DC electrical

| Quantity | Terminal | Front end | Scaling | MCU pin |
|---|---|---|---|---|
| Battery voltage | J1-1 (battery +) | 100k / 22k divider | ÷5.55 → 16 V uses most of the ADC span | PA6 |
| Charge alternator D+ | J13 | 120 Ω 3 W excitation + 100k / 22k sense | ÷5.55; excitation supplies the ~100 mA the alternator needs to self-excite | PA7 |

D+ is both a **measurement and an output**: the 120 Ω resistor feeds current
into the alternator's field to start it charging, and the same node is sensed
to detect charge failure (D+ below `charge_fail_ratio` × battery volts).

## 4. Measured — digital inputs

Eight identical channels on J2 (plus two GND poles). Divider 5.6k/1.8k from
12 V, BAT54S clamp, 1 µF debounce cap (τ ≈ 1.4 ms) plus a firmware 3-sample
debounce. Wetting current ≈1.6 mA — enough to break oxide on a dirty panel
contact. Each channel has a per-channel solder jumper (JP4–JP11) that fits a
2.2 k pull-up instead, so the channel accepts a **ground-switched** contact.
Fit the pull-up or the field 12 V feed, never both.

| Ch | MCU pin | Firmware field | Function |
|---|---|---|---|
| DIN1 | PE0 | `emergency_stop` | E-stop mushroom, **fail-safe** (wired so a broken wire reads as pressed) |
| DIN2 | PE1 | `remote_start` | External start command (BMS / building controller) in AUTO |
| DIN3 | PE2 | `low_oil_switch` | Engine's own low-oil-pressure switch |
| DIN4 | PE3 | `high_coolant_switch` | Engine's own high-coolant-temperature switch |
| DIN5 | PE4 | `low_coolant_level` | Coolant level float |
| DIN6–8 | PE5–PE7 | — | **Spare.** Wired to the MCU, no firmware function assigned |

**The channel numbers above are not yet fixed anywhere.** The board is generic
— eight identical channels — and `gcu_inputs_t` names the five functions that
are used, but the mapping between them lives in the HAL layer, which does not
exist yet. The order shown is the struct order and is the sensible default;
confirm it when the HAL is written, and label the panel to match.

## 5. Measured — over J1939 from the engine ECU

Listener only. Every value is aged; once its age passes its timeout the value
is marked invalid rather than going stale.

| PGN | Name | SPN | Quantity | Resolution | Timeout |
|---|---|---|---|---|---|
| 61444 | EEC1 | 190 | Engine speed | 0.125 rpm/bit | 500 ms |
| 65263 | EFL/P1 | 100 | Oil pressure | 4 kPa/bit | 2 s |
| 65262 | ET1 | 110 | Coolant temperature | 1 °C/bit, −40 offset | 3 s |
| 65276 | DD | 96 | Fuel level | 0.4 %/bit | 5 s |
| 65226 | DM1 | — | Red (stop) lamp, amber (warning) lamp, first DTC (SPN + FMI) | — | 3 s |
| 60928 | AC | — | Address claim — contention resolved by lower NAME | — | — |
| 59904 | RQST | — | Request; answered for the address-claim PGN | — | — |

Single-frame DM1 only: TP.BAM multi-DTC transfers are not reassembled, but the
lamp bits still carry the severity, which is what drives the shutdown.

**Comms loss** is only an alarm once run-enable has been on past a 2 s boot
grace — with K1 open the engine ECU is asleep and silence is normal.

## 6. Measured — operator inputs

From the display board over the shared SPI bus (74HC165 key shift register):
`key_start`, `key_stop` (also resets latched alarms), `mode_auto`.

## 7. Derived — computed, not sensed

| Value | Source |
|---|---|
| Generator / mains frequency | zero-crossing or period measurement on the L1 voltage channel |
| RMS volts and amps | true RMS over a window on the sampled waveforms |
| Total real power, power factor | `mean(v×i)` on phase-matched channels — the voltage and CT filters are phase-matched to ≈0.1° precisely so this cancels |
| Run hours | accumulated in firmware, stored in the M95M02 EEPROM |
| Engine state, AMF state | `engine_fsm.c`, `amf_fsm.c` |
| 21 alarm conditions | `protection.c` (see below) |

**Not yet implemented:** `real_power_w` and `power_factor` are declared in
`gcu_inputs_t` and published on Modbus registers 19/20, but nothing fills them
— there is no AC sampling layer yet, so both read zero. That is deliberate: a
plausible-looking constant PF in a SCADA trend is worse than an obvious zero.

### Alarms evaluated every 10 ms tick

`EMERGENCY_STOP`, `LOW_OIL_PRESSURE`, `HIGH_COOLANT_TEMP`, `OVERSPEED`,
`UNDERSPEED`, `GEN_UNDER_VOLT`, `GEN_OVER_VOLT`, `GEN_UNDER_FREQ`,
`GEN_OVER_FREQ`, `OVERCURRENT`, `CHARGE_FAIL`, `BATT_LOW`, `BATT_HIGH`,
`LOW_COOLANT_LEVEL`, `FAIL_TO_START`, `FAIL_TO_STOP`, `SENSOR_LOSS`,
`ECU_RED_LAMP`, `ECU_WARNING`, `ECU_COMMS_LOST` — each classified warning or
shutdown, most with a configurable threshold and qualification delay.

---

## 8. Controlled — relay outputs

Six G5LE-1-DC12 relays, low-side driven by 2N7002K FETs with SS34 flyback
diodes. Coils run from +12V_SW (33.3 mA each, 200 mA total).

**Every contact is dry and pilot-duty.** No field current crosses the PCB.

| Relay | MCU pin | Net | Contact goes to | Drives |
|---|---|---|---|---|
| K1 FUEL | PD2 | `RLY_FUEL` | J8-2 | Engine ECU **run enable** (CRDi) or the fuel solenoid (legacy) |
| K2 START | PD3 | `RLY_START` | J8-3 | **Pilot only** — the coil of an external starter relay |
| K3 GEN | PD4 | `RLY_GEN` | J15 volt-free pair | Generator contactor coil |
| K4 MAINS | PD5 | `RLY_MAINS` | J15 volt-free pair | Mains contactor coil |
| K5 AUX1 | PD6 | `RLY_AUX1` | J8-4 | Configurable, default **horn** |
| K6 AUX2 | PD7 | `RLY_AUX2` | J8-5 | Configurable, default **preheat** |

**J8 pins 1 and 6 are OUT_COM** — the common contact supply, which the
*installer* feeds from their own fused source. K1/K2/K5/K6 contact COMs come
from that, not from the board rail.

**START must never drive a starter motor solenoid directly.** A 12 V solenoid
pulls 20–40 A and glow plugs 28–60 A, against 8 A DC contacts. The contacts
would weld — and a welded starter contact means an engine that cranks and
cannot be commanded to stop.

**K3/K4 are volt-free pairs (COM + NO) on their own block J15**, because
contactor coils run from their own AC source and the panel's hardware interlock
must stay load-bearing. J15 and J5/J6 are 8-pole bodies with only the odd poles
wired, giving 10.16 mm live-to-live pitch for creepage.

K5/K6 select from `gcu_aux_fn_t`: `AUX_OFF`, `AUX_HORN`, `AUX_PREHEAT`,
`AUX_RUNNING`, `AUX_FAULT`.

## 9. Controlled — other outputs

| Output | MCU pin | Purpose |
|---|---|---|
| D+ excitation | (analog) | ~100 mA into the charge alternator field via 120 Ω 3 W |
| Display backlight PWM | PE11 (TIM1_CH2) | Dimming; also the intended load-shed during crank |
| TFT reset | PE8 | RA8875 display controller reset |
| TFT / LED / key chip selects | PB7 / PB8 / PB9 | Shared SPI1 bus to the display board |
| LED shift register | via PB8 | Panel indicator LEDs (74HC595) |
| EEPROM chip select | PB12 | M95M02 config + run hours (SPI2) |
| RS485 direction | PD10 | DE for the Modbus half-duplex turnaround |
| Heartbeat LED | PA8 | Board-alive indicator |

## 10. Controlled — remotely, over Modbus RTU

RS485 on J10. Four holding registers are the entire remote control surface —
deliberately small.

| Reg | Function |
|---|---|
| 0 | Mode: 0 off, 1 manual, 2 auto, 3 test |
| 1 | Remote start request (0/1) |
| 2 | Alarm reset — write 1 to pulse, always reads back 0 |
| 3 | Lamp test (0/1) |

23 input registers publish the live measurement snapshot read-only: gen and
mains volts ×3, currents ×3, frequency, rpm, oil, coolant, fuel, battery,
engine state, AMF state, a 32-bit alarm bitmap, real power, power factor and
run hours. Full map in `firmware/core/modbus.h`.

---

## What we control on the engine ECU

This is the question worth being precise about, because a common-rail engine
ECU manages its own injection, timing, rail pressure, EGR and cold-start aid.
ECU-25 does not touch any of that. What it actually has is:

**Physical authority — two contacts:**

- **Run enable (K1).** The single most important output. Closed, the engine ECU
  is permitted to run; open, it stops. On this engine that is an ECU enable
  input; on a legacy mechanical engine the same contact drives the fuel
  solenoid. K1 energizes from the PREHEAT state onward, not just at crank, to
  give the ECU its boot time before the starter engages.
- **Starter pilot (K2).** Energizes an external starter relay. Crank-disconnect
  is decided here — on rpm crossing `crank_disconnect_rpm` or oil pressure
  crossing `crank_disconnect_oil_bar`, whichever comes first.

**Over CAN — nothing.** ECU-25 transmits only its own J1939 address claim. It
issues no TSC1 torque/speed command, no shutdown request, no mode change. It
reads the engine's broadcasts and reacts by opening K1. If the CAN cable is cut
the engine keeps running exactly as before; only the data goes away, and
`ECU_COMMS_LOST` raises.

**What the engine ECU owns, and we deliberately do not:**
injection quantity and timing, rail pressure, idle governing, speed droop, EGR,
DOC management, glow-plug / cold-start aid (which is why K6 preheat is
configurable rather than hard-wired), and its own protection shutdowns — those
reach us as DM1 lamp bits, and a red lamp makes us open K1.

**Everything else the controller commands is switchgear, not engine control:**
the generator contactor, the mains contactor, and the AMF transfer sequence
between them (break-before-make, with a dead time, and the panel's mechanical
interlock still the final authority).

---

## Connector summary

| Ref | Poles | Carries |
|---|---|---|
| J1 | 2 | Battery + / − |
| J2 | 10 | DIN1–8 + 2× GND |
| J3 | 4 | Oil, fuel, temp senders + return |
| J4 | 2 | Magnetic pickup |
| J5 | 8 (odd wired) | Generator L1 L2 L3 N — **415 V** |
| J6 | 8 (odd wired) | Mains L1 L2 L3 N — **415 V** |
| J7 | 6 | CT1–3 S1/S2 |
| J8 | 6 | OUT_COM, FUEL, START, AUX1, AUX2, OUT_COM |
| J9 | 3 | CAN H / L / shield |
| J10 | 3 | RS485 A / B / shield |
| J13 | 2 | Charge alternator D+ |
| J15 | 8 (odd wired) | GEN and MAINS contactor volt-free pairs |
| J12 | 20-way ribbon | Display board — SPI, selects, backlight power |
| J14 | 3 | Debug UART (USART1) |
| J11 | TC2030 | SWD programming/debug |
