# ECU-25 — Genset AMF Controller: Design Specification

**Date:** 2026-07-29 (rev A 2026-08-11: engine confirmed common-rail electronic)
**Status:** Draft for review
**Target:** Kirloskar KG4-25WS1, 25 kVA diesel genset, **12 V DC / 75 Ah** electrical system, 415 V 3-phase alternator, 50 Hz / 1500 RPM. Engine is a **common-rail electronic diesel with its own engine-management ECU** (engine 3R550ETA 4G1, CPCB IV+: 3-cyl 1.65 L CRDi, EGR + DOC — no SCR/DEF and no DPF, so no aftertreatment UI is needed); ECU-25 is the genset supervisor talking to it over J1939. A legacy mode (mechanical-governor engine, analog senders, fuel solenoid) is retained so the same hardware also runs older gensets.

## 1. Purpose

An original, production-style genset controller (engine control unit + auto-mains-failure controller) designed from first principles. Goals: a working prototype that can start, run, protect, and meter a real 25 kVA diesel genset, and serve as a learning vehicle for automotive-grade hardware and firmware design. This is a new design — it does not copy or imitate any manufacturer's proprietary product.

## 2. Scope

In scope:
- Engine start/stop sequencing and protection for a mechanical-governor diesel engine.
- 3-phase AC metering of both generator and mains supplies.
- Auto-mains-failure (AMF) operation with break-before-make load transfer.
- Front-panel display and keypad, plus CAN (J1939) and RS485 (Modbus RTU) communications.

Out of scope (this revision):
- Electronic governor speed control (one spare PWM output is reserved on the connector for a future revision).
- Synchronizing / paralleling, AVR control, remote telemetry (GSM/Ethernet).
- Certification (CE/EMC compliance testing). The design follows EMC-aware practices but is not certified.

## 3. System Architecture

Two boards:

```
┌─────────────────────────────────────────┐
│ DISPLAY BOARD (2-layer)                 │
│ 4.3" 480x272 color TFT + RA8875 (SPI)   │
│ Keys: STOP | AUTO | MANUAL | START      │
│       ▲ ▼ ⏎ (menu)                      │
│ 8 status LEDs                           │
└──────────────┬──────────────────────────┘
               │ 20-way ribbon (SPI + GPIO, 3.3 V + 5 V)
┌──────────────┴──────────────────────────┐
│ MAIN BOARD (4-layer, ~120 x 100 mm)     │
│ Power supply │ STM32F407VGT6 │ EEPROM   │
│ 8x digital in │ 3x sender in │ RPM in   │
│ 6x AC voltage in │ 3x CT in │ 6x relay  │
│ CAN (J1939) │ RS485 (Modbus) │ SWD      │
└─────────────────────────────────────────┘
```

## 4. Hardware Design — Main Board

### 4.1 Power supply
- Input 8–16 V DC continuous (12 V nominal); survives cranking dips and clamps load-dump transients.
- Chain: input connector → 5 A blade fuse → SMCJ16CA bidirectional TVS → reverse-polarity P-channel MOSFET → ferrite bead + bulk capacitance → LM5164-Q1 synchronous buck (100 V max input) → 5 V rail → TLV75533 LDO → 3.3 V rail.
- +12V_SW powers the relay coils; contacts are dry, analog pull-ups, LCD backlight; 3.3 V powers MCU, transceivers, analog front ends.
- Bulk electrolytic sized to ride through ≥10 ms crank transients without MCU reset.

### 4.2 MCU core
- STM32F407VGT6 (LQFP100, 168 MHz, triple 12-bit ADC, 2x CAN, FPU).
- 8 MHz HSE crystal; 32.768 kHz LSE crystal for RTC, backed by supercapacitor.
- M95M02-DR SPI EEPROM (256 KB) for configuration and fault log.
- SWD debug header (10-pin Cortex); BOOT0 strap; NRST with RC + test point.
- Internal independent watchdog (IWDG) always enabled in firmware.

### 4.3 Digital inputs (8x)
- 12 V level, configurable active-high/low in firmware.
- Per channel: series resistor divider, Schottky clamp to rail, RC filter (~1 ms), Schmitt buffer or direct GPIO with firmware debounce.
- Default assignment: emergency stop, low oil pressure switch, high coolant temperature switch, remote start, low coolant level, 3x configurable spare.

### 4.4 Analog sender inputs (3x) + DC measurements
- Resistive senders: oil pressure (VDO 10–184 Ω), coolant temperature (NTC), fuel level (0–190 Ω).
- Per channel: precision pull-up current source from 5 V, RC anti-alias filter, clamp, ADC channel. Sender curves configurable in firmware (table interpolation).
- Battery voltage: protected divider to ADC.
- Charge alternator D+: excitation resistor from switched supply + divider to ADC for charge-fail detection.

### 4.5 RPM input
- Magnetic pickup (MPU): AC-coupled input, LM2903 comparator with hysteresis, output to STM32 timer input capture.
- Alternative source: W-terminal of charge alternator (same conditioning path, jumper-selectable).
- Flywheel teeth count configurable in firmware.

### 4.6 AC voltage sensing (6 channels)
- Generator L1/L2/L3 and mains L1/L2/L3, measured line-to-neutral, up to 300 Vrms per channel.
- Per channel: high-impedance divider (~1.3 MΩ total, built from ≥4 series 1206 resistors for voltage rating and creepage), biased to 1.65 V mid-rail, RC anti-alias filter, into ADC.
- AC input nets routed with ≥3 mm clearance from logic; AC terminal block physically separated on the board edge.

### 4.7 Current transformer inputs (3x)
- Standard 5 A secondary CTs, external, user-supplied.
- Per channel: low-ohm burden resistor on board, mid-rail bias, RC filter, ADC channel. CT ratio configurable.

### 4.8 Relay outputs (6x)
- K1 engine run enable (16 A) — powers the engine ECU's run/keyswitch line on electronic engines, or the fuel solenoid directly on legacy engines. K2 starter motor (16 A) — hardwired crank on both engine types. K3 generator contactor coil, K4 mains contactor coil, K5 alarm/horn, K6 preheat/glow (8 A each, 250 VAC contacts on K3/K4).
- Each: logic-level N-FET driver, coil flyback diode, contact wiring to pluggable terminal blocks. K3/K4 firmware-interlocked and contact-interlocked externally (wiring note in manual).

### 4.9 Communications
- CAN: TJA1051T/3 transceiver, 120 Ω split termination (jumper), for J1939.
- RS485: THVD1450 transceiver, fail-safe biasing + termination jumpers, Modbus RTU slave.
- One spare timer PWM output brought to the I/O connector (future electronic-governor actuator drive, external power stage).

### 4.10 Connectors
- Pluggable screw-terminal blocks (5.08 mm pitch) grouped: DC power, digital inputs, senders/RPM, relay contacts, AC voltage inputs, CTs, comms.
- Display ribbon: 20-way boxed IDC.

## 5. Hardware Design — Display Board

- 4.3" 480x272 color TFT (phone-size, modern-controller class) driven by an RA8875 graphics controller: own framebuffer RAM and hardware drawing, so the MCU talks plain SPI — no parallel RGB bus, no MCU change.
- LED backlight powered from the 5 V rail (module driver), PWM-dimmable from the MCU.
- 7 tactile keys: STOP, AUTO, MANUAL, START, UP, DOWN, ENTER (physical keys — glove-friendly; no touchscreen).
- 8 LEDs: Mains OK, Gen Running, Load on Mains, Load on Gen, Warning, Shutdown, Auto mode, Charge.
- Keys and LEDs on 74HC165/74HC595 shift registers sharing the SPI bus; ribbon grows to 20 ways (3V3, 5V, GND, SPI, RA8875 CS/INT/RST, backlight PWM, strobes).
- 2-layer PCB sized as a front panel (~140 x 100 mm bezel).

## 6. Firmware Architecture

Platform: STM32 HAL + FreeRTOS, C, built with arm-none-eabi-gcc + CMake; unit tests for pure-logic modules run on host (ceedling or plain CTest).

Modules (each with a defined interface, testable in isolation):
- **engine_fsm** — states: STOPPED → PREHEAT → CRANK → CRANK_REST (max 3 attempts) → RUNNING_WARMUP → RUNNING → COOLDOWN → STOPPING → STOPPED. Crank disconnect on RPM threshold OR oil-pressure rise.
- **protection** — evaluates all channels every 10 ms; two classes: warning (alarm only) and shutdown (immediate fuel cut + latched alarm). Covers: low oil pressure, high coolant temp, over/underspeed, gen over/under-voltage, gen over/under-frequency, overcurrent, charge fail, battery high/low, e-stop, fail-to-start, fail-to-stop.
- **amf_fsm** — mains monitor (volt + freq window, qualification timers) → auto start → transfer to gen (break-before-make, transfer delay) → mains-return qualification → retransfer → cooldown → stop.
- **metering** — triple-ADC simultaneous sampling via DMA, 128 samples/cycle tracked to measured frequency; per-channel true-RMS, frequency (zero-cross with interpolation), active/apparent power, PF, energy accumulators, engine hours.
- **senders** — sender resistance → engineering units via configurable interpolation tables.
- **hmi** — UI built with LVGL (pages: status, metering, alarms, engine data/DM1, config), rendered via RA8875 over SPI; key handling, LED states. Full UI runs in LVGL's PC simulator for development and review before hardware exists.
- **comms_modbus** — RTU slave, holding/input register map documented in `docs/modbus-map.md`.
- **comms_j1939** — the primary engine interface on electronic engines: address claim; read engine data (EEC1 — speed, ET1 — coolant temp, engine fluid level/pressure — oil pressure, hours, DM1 — active fault codes with lamp status, decoded to the display and fault log); broadcast genset PGNs (AC volts/amps/frequency/power). Engine speed, oil pressure, and coolant temperature from J1939 feed the same protection engine as the analog paths.
- **engine source selection** — config parameter chooses per-signal source: J1939 (electronic engine) or analog sender/MPU (legacy engine). Crank disconnect uses J1939 speed when available, MPU otherwise. Analog channels double as a backup/plausibility cross-check when J1939 is primary.
- **config** — parameter store in EEPROM, versioned, defaults on first boot, CRC-protected.
- **faultlog** — circular log in EEPROM: event, engine-hours timestamp, RTC timestamp, snapshot of key values.

Timing: 1 kHz base tick; protection loop 100 Hz; metering DMA continuous; HMI 20 Hz; IWDG kicked only when all tasks check in (task-alive bitmask).

## 7. Safety and Test Plan

1. **Board bring-up** — power rails, MCU alive, peripherals, no AC connected.
2. **Bench rig** — 12 V bench PSU; signal generator as MPU; potentiometers as senders; switches as digital inputs; lamps/relays as loads; 230 V through an isolation transformer for ONE AC channel to validate metering math; CT loop test with a load and multi-turn primary.
3. **Genset dry runs** — fuel solenoid held closed: verify crank, crank-disconnect inhibit, e-stop.
4. **Commissioning checklist** — staged: engine control only → add gen metering → add protections live → AMF transfer last, with manual supervision at every stage.

Safety rules (non-negotiable): all 415 V work de-energized when wiring; AC terminals shrouded; board never handled while mains sensing is live; contactor hardware interlock mandatory in the panel regardless of firmware interlock.

## 8. Deliverables and Repository Layout

```
ecu/
├── docs/            specs, Modbus map, J1939 PGN list, test plans, manual
├── hardware/
│   ├── main-board/     KiCad project (hierarchical sheets, 4-layer)
│   └── display-board/  KiCad project (2-layer)
└── firmware/        CMake + HAL + FreeRTOS project, host-run unit tests
```

Schematic conventions: hierarchical sheets (Power, MCU, Digital In, Analog In, AC Sense, CT, Relays, Comms, Connectors); reference designators grouped per sheet (R1xx power, R2xx MCU, ...); net names VBAT, +5V, +3V3, AGND, CAN_H/L, 485_A/B; title blocks and revision table on every sheet.

## 9. Build Order

1. KiCad main-board schematic, sheet by sheet (power first), with design review per sheet.
2. Display-board schematic.
3. Main-board 4-layer layout (AC/logic partitioning rules), then display layout.
4. ERC/DRC, BOM, gerbers.
5. Firmware skeleton + host-tested logic modules (engine_fsm, protection, amf_fsm first).
6. Board bring-up firmware, then full application.
7. Bench rig testing, then staged genset commissioning.
