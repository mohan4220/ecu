# ECU-25 KiCad project

`ecu25-main/` is the main-board schematic: a KiCad 7 hierarchical project with
one sheet per subsystem. Open `ecu25-main/ecu25-main.kicad_pro` in KiCad.

Component values come from the reviewed calculation sheet in
[docs/circuits/README.md](../../docs/circuits/README.md) — that file is the
design authority; this project is its capture.

## Sheets

| Sheet | Contents |
|-------|----------|
| Power | Input protection (F1, SMCJ33CA, reverse-polarity P-FET), LM5164 buck 24→5 V, TLV75533 LDO 5→3.3 V, PTC-protected +24V_SW rail |
| Analog inputs | 3× current-source sender inputs (LM358 + BC857, 8 mA / 2 mA), battery sense ÷15.7, D+ excite + sense |
| AC sensing | 6× ÷237 dividers biased at VREF_MID, 3× CT burden + MCP6002 amps, 1.65 V buffered midpoint |
| Digital inputs + MPU | 8× 24 V inputs (divider + BAT54S clamp, per-channel GND-side-switch jumper), MPU → LM2903 comparator with hysteresis |
| Relay drivers | 6× G5LE-1 + 2N7002K + SS34; GEN/MAINS contacts volt-free |
| Comms | TJA1051T/3 CAN (split termination, JP1), THVD1450 RS485 (JP2 term), M95M02 EEPROM |
| MCU | STM32F407VGT6, crystals, VCAP, VDDA filter, supercap RTC backup, SWD (TC2030), debug UART |
| Display connector | 20-way ribbon to the display board (SPI + selects + backlight power) |

## MCU pin assignment

ADC: PA0-2 = CT1-3, PA3-7 = oil/fuel/temp/vbat/D+, PC0-5 = gen/mains L1-3.
SPI1 (display) on PB3/4/5, selects PB7/8/9. SPI2 (EEPROM) PB12-15.
CAN1 PD0/PD1. USART3 + DE (RS485) PD8/9/10. USART1 (debug) PA9/10.
Relays PD2-7. Digital inputs PE0-7. TFT RST/INT PE8/9, backlight PWM PE11
(TIM1_CH2). MPU capture PB6 (TIM4_CH1). Heartbeat LED PA8.

## Regenerating

The schematics are generated (and re-generated) by script — do not hand-edit
the `.kicad_sch` files, change the generator instead:

```
.venv/bin/python hardware/kicad/gen/gen_ecu25.py
```

`gen/kisch.py` is the builder library (symbol flattening, pin-exact placement,
wire splitting at T-joins). Validation used during generation:

```
kicad-cli sch export netlist hardware/kicad/ecu25-main/ecu25-main.kicad_sch -o /tmp/ecu25.net
kicad-cli sch export svg hardware/kicad/ecu25-main/ecu25-main.kicad_sch -o /tmp/svgs
```

Run ERC from eeschema (Inspect → Electrical Rules Checker) — kicad-cli 7 has
no ERC command.

## PCB

Footprints are assigned in the schematic generator (`footprint_for()` in
`gen_ecu25.py`): 0603 passives by default, wattage/voltage exceptions by value
(1206 AC chain, 1210 0.5 W, 2512 shunts, THT power parts), MKDS-3 terminal
blocks for battery/AC/CT/relay field wiring, MKDS-1,5 for signal-level.

`ecu25-main/ecu25-main.kicad_pcb` is a generated **starting point**, not a
routed board: 200×150 mm 2-layer outline, M3 corner holes, field connectors on
the edges (battery/MPU/D+/senders left, fuse + display ribbon + GEN/MAINS AC
top, CT right, DIN / comms / relay outputs / contactor pairs bottom), relays
in a 2×3 block with the volt-free GEN/MAINS pair above J15, MCU central, buck
hot loop and CT front-ends at fixed positions, remaining parts shelf-packed
inside their subsystem zone, all pads on nets, GND pour both layers with
keepouts under the 415 V regions. J5/J6/J15 are 8-pole blocks with only the
odd poles wired (pad remap in `gen_pcb.py`) — 10.16 mm live pitch for
creepage. Regenerate:

```
kicad-cli sch export netlist hardware/kicad/ecu25-main/ecu25-main.kicad_sch -o /tmp/ecu25.net
python3 hardware/kicad/gen/gen_pcb.py /tmp/ecu25.net   # system python (needs pcbnew)
```

`ecu25-main.kicad_dru` carries the HV clearance rules (3.0 mm for the J5/J6
phase nets and the J15 volt-free contactor nets — GEN and MAINS are
unsynchronized sources, so phase-to-phase can reach ~680 Vpk; 1.0 mm inside
the 4×330k divider chains; per-chain exemptions so the first 330k's own pads
don't false-trip) — pcbnew picks it up automatically. The file is generated:
re-annotating nets by hand orphans the rules, regenerate instead. Routing
guidance: battery input F1→Q1→buck ≥2 mm track; +24V_SW and relay
coil/contact tracks ≥1 mm; 0.05 R CT shunts want wide copper spades and
Kelvin-routed sense off the pad inner edges; VREF_MID is a star net — route
each channel's bias from the buffer side. Interactive routing (or freerouting
via DSN export) is the remaining manual step, then DRC in pcbnew.
