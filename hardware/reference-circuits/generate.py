#!/usr/bin/env python3
"""Generate ECU-25 reference circuit diagrams (SVG + PNG) into docs/circuits/.

These are design references with calculated values — the source of truth
until the KiCad schematics are drawn. Run: .venv/bin/python generate.py
"""
import os

import schemdraw
import schemdraw.elements as elm
from schemdraw import flow

schemdraw.use("matplotlib")

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "circuits")
os.makedirs(OUT, exist_ok=True)


def save(d, name):
    d.save(os.path.join(OUT, name + ".svg"))
    d.save(os.path.join(OUT, name + ".png"), dpi=150)
    print("wrote", name)


# ---------------------------------------------------------------- 1. input protection
def input_protection():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.2)
    d += elm.Line().right(0.001).label("J1-1\nVBAT +24V", loc="left")
    d += elm.Dot()
    d += elm.Fuse().right().label("F1 5A\nblade", loc="top")
    d += (n1 := elm.Dot())
    d += elm.Zener().down().label("D1\nSMCJ33CA\nTVS", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n1.start).right(2.0)
    d += (q1 := elm.PFet(bulk=True).theta(-90).anchor("drain"))
    d += elm.Label().at((q1.drain[0] + 0.75, q1.drain[1] + 1.1)).label("Q1 SQJ457EP\n-60V P-FET")
    d += elm.Line().at(q1.source).right(0.8)
    d += (sj := elm.Dot())
    # gate network below the FET
    d += elm.Line().at(q1.gate).down(0.5)
    d += (g2 := elm.Dot())
    d += elm.Line().at(g2.start).down(0.8)
    d += elm.Resistor().down(2.0).label("R1\n100k", loc="bottom")
    d += elm.Ground()
    d += elm.Zener().at(g2.start).right().tox(sj.start).label("D2 15V\nBZT52C15", loc="bottom")
    d += elm.Line().up().toy(sj.start)
    # ferrite + caps
    d += elm.Inductor2(loops=2).at(sj.start).right().label("FB1 ferrite\n600Ω@100MHz 3A", loc="top")
    d += (n3 := elm.Dot())
    d += elm.Capacitor(polar=True).down().label("C1\n100µF 50V", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n3.start).right(2.0)
    d += (n4 := elm.Dot())
    d += elm.Capacitor().down().label("C2\n100nF 50V", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n4.start).right(1.0)
    d += elm.Arrow().right(0.8).label("+24V_PROT\nto buck", loc="right")
    save(d, "01-power-input-protection")


# ---------------------------------------------------------------- 2. buck converter
def buck():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.2)
    ic = elm.Ic(
        pins=[
            elm.IcPin(name="VIN", side="left", slot="3/3"),
            elm.IcPin(name="EN", side="left", slot="2/3"),
            elm.IcPin(name="RON", side="left", slot="1/3"),
            elm.IcPin(name="BST", side="right", slot="3/3"),
            elm.IcPin(name="SW", side="right", slot="2/3"),
            elm.IcPin(name="FB", side="right", slot="1/3"),
            elm.IcPin(name="GND", side="bottom"),
        ],
        edgepadW=1.4,
        edgepadH=1.0,
        pinspacing=2.0,
        label="U1  LM5164-Q1",
        lblloc="top",
    )
    d += ic
    d += elm.Ground().at(ic.GND)
    # VIN rail with input cap
    d += elm.Line().at(ic.VIN).left(1.6)
    d += (vin := elm.Dot())
    d += elm.Line().left(1.0).label("+24V_PROT", loc="left")
    d += elm.Capacitor().at(vin.start).down().label("C3\n2.2µF 100V", loc="bottom")
    d += elm.Ground()
    # EN/UVLO divider in its own column, further left
    d += elm.Line().at(ic.EN).left(3.6)
    d += (en := elm.Dot())
    d += elm.Resistor().at(en.start).up().toy(vin.start).label("R2\n100k")
    d += elm.Line().tox(vin.start)
    d += elm.Dot()
    d += elm.Resistor().at(en.start).down(2.4).label("R3\n23.2k\n(UVLO 8V)", loc="bottom")
    d += elm.Ground()
    # RT
    d += elm.Line().at(ic.RON).left(0.8)
    d += elm.Resistor().down(2.0).label("R4 100k (RON)\n≈300kHz", loc="bottom")
    d += elm.Ground()
    # BST cap
    d += elm.Line().at(ic.BST).right(1.4)
    d += elm.Capacitor().down().toy(ic.SW).label("C4\n100nF", loc="bottom")
    d += (bstsw := elm.Dot())
    # SW node + inductor
    d += elm.Line().at(ic.SW).tox(bstsw.start)
    d += elm.Inductor2(loops=3).right().label("L2 33µH 2A shielded", loc="top")
    d += (vout := elm.Dot())
    d += elm.Capacitor().down().label("C5,C6\n2×22µF 25V", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(vout.start).right(2.6)
    d += (vo2 := elm.Dot())
    d += elm.Arrow().right(0.8).label("+5V @1A", loc="right")
    # FB divider
    d += elm.Resistor().at(vo2.start).down(2.2).label("R5\n38.3k 1%", loc="bottom")
    d += (fb := elm.Dot())
    d += elm.Resistor().down(2.2).label("R6\n12.1k 1%", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(fb.start).tox(ic.FB)
    d += elm.Line().toy(ic.FB)
    d += elm.Line().to(ic.FB)
    save(d, "02-buck-24v-to-5v")


# ---------------------------------------------------------------- 3. LDO
def ldo():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.2)
    ic = elm.Ic(
        pins=[
            elm.IcPin(name="IN", side="left", slot="2/2"),
            elm.IcPin(name="EN", side="left", slot="1/2"),
            elm.IcPin(name="OUT", side="right", slot="2/2"),
            elm.IcPin(name="GND", side="bottom"),
        ],
        edgepadW=1.2,
        edgepadH=1.0,
        pinspacing=2.0,
        label="U2  TLV75533\n3.3V fixed, 500mA",
        lblloc="top",
    )
    d += ic
    d += elm.Ground().at(ic.GND)
    d += elm.Line().at(ic.IN).left(1.4)
    d += (vin := elm.Dot())
    d += elm.Line().left(0.8).label("+5V", loc="left")
    d += elm.Capacitor().at(vin.start).down().label("C7\n2.2µF", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(ic.EN).left(0.7)
    d += elm.Line().toy(vin.start)
    d += elm.Line().tox(vin.start)
    d += elm.Dot()
    d += elm.Line().at(ic.OUT).right(1.4)
    d += (vout := elm.Dot())
    d += elm.Capacitor().down().label("C8\n10µF", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(vout.start).right(1.6)
    d += elm.Arrow().right(0.8).label("+3V3\nMCU + analog\n+ display logic", loc="right")
    save(d, "03-ldo-5v-to-3v3")


# ---------------------------------------------------------------- 4. digital input
def digital_input():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.2)
    d += elm.Line().right(0.001).label("DIN1 terminal\n(switch to +24V\nor GND, config)", loc="left")
    d += (t0 := elm.Dot())
    d += elm.Resistor().at(t0.start).right().label("R10\n10k 0.5W")
    d += (n1 := elm.Dot())
    d += elm.Resistor().down().label("R11\n3.3k", loc="bottom")
    d += elm.Ground()
    d += elm.Resistor().at(t0.start).up(2.4).label("R13 10k + JP4\n(fit for GND-side\nswitches)", loc="top")
    d += elm.Line().up(0.4).label("+24V_PROT", loc="top")
    d += elm.Line().at(n1.start).right(1.6)
    d += (n2 := elm.Dot())
    d += elm.Capacitor().down().label("C10\n470nF\nτ≈1.2ms", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n2.start).right(0.8)
    d += elm.Resistor().right().label("R12\n10k")
    d += (n3 := elm.Dot())
    d += elm.Diode().at(n3.start).up(1.8).label("D10a", loc="bottom")
    d += elm.Vdd().label("+3V3")
    d += elm.Diode().at((n3.start[0], n3.start[1] - 1.8)).up(1.8).label("D10b\nBAT54S", loc="bottom")
    d += elm.Ground().at((n3.start[0], n3.start[1] - 1.8))
    d += elm.Line().at(n3.start).right(1.0)
    d += elm.Arrow().right(0.8).label("MCU GPIO\n(fw debounce)", loc="right")
    save(d, "04-digital-input")


# ---------------------------------------------------------------- 5. sender input
def sender_input():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    d += elm.Vdd().at((0, 5.4)).label("+5V")
    d += elm.SourceI().at((0, 5.4)).down(2.4).label(
        "I1  8mA (oil/fuel)\n249Ω→2mA (temp)\n[LM358+BC857+62Ω\nsee KiCad sheet]", loc="bottom"
    )
    d += elm.Line().down(0.6)
    d += (n := elm.Dot())
    d += elm.Line().at(n.start).left(1.6).label("sender\nterminal", loc="left")
    d += elm.ResistorVar().at(n.start).down(2.6).label("VDO sender\n10–184Ω oil\nNTC temp\n0–190Ω fuel", loc="bottom")
    d += elm.Ground().label("engine block", loc="bottom")
    d += elm.Line().at(n.start).right(1.2)
    d += elm.Resistor().right().label("R23\n1k")
    d += (f1 := elm.Dot())
    d += elm.Capacitor().down(2.0).label("C20\n100nF", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(f1.start).right(0.6)
    d += elm.Resistor().right().label("R24\n4.7k")
    d += (n3 := elm.Dot())
    d += elm.Diode().at(n3.start).up(1.8).label("D20\nBAV199", loc="bottom")
    d += elm.Vdd().label("+3V3")
    d += elm.Line().at(n3.start).right(0.8)
    d += elm.Arrow().right(0.8).label("ADC\nR = V/I", loc="right")
    save(d, "05-sender-input")


# ---------------------------------------------------------------- 6. MPU / RPM
def mpu():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    d += elm.SourceSin().up(2.0).label("MPU\n±0.5V…±50V", loc="left").reverse()
    d += elm.Capacitor().right().label("C30\n100nF 100V")
    d += elm.Resistor().right().label("R30\n10k")
    d += (n1 := elm.Dot())
    d += elm.Resistor().at(n1.start).up(2.4).label("R31\n100k")
    d += elm.Vdd().label("+3V3")
    d += elm.Resistor().at(n1.start).down(2.4).label("R32\n100k", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n1.start).right(1.8)
    d += (n2 := elm.Dot())
    d += elm.Diode().at(n2.start).up(1.7).label("D30a", loc="bottom")
    d += elm.Vdd().label("+3V3")
    d += elm.Diode().at((n2.start[0], n2.start[1] - 1.7)).up(1.7).label("D30b\nBAV99", loc="bottom")
    d += elm.Ground().at((n2.start[0], n2.start[1] - 1.7))
    d += elm.Line().at(n2.start).right(2.6)
    op = elm.Opamp(leads=True).anchor("in2").label("U3 LM2903\nV+ = +5V (VICM limit)", loc="bottom")
    d += op
    # inverting input reference (1.65V)
    d += elm.Line().at(op.in1).left(0.8)
    d += (r := elm.Dot())
    d += elm.Resistor().at(r.start).up(2.2).label("R33\n10k")
    d += elm.Vdd().label("+3V3")
    d += elm.Line().at(r.start).down(1.6)
    d += elm.Resistor().down(2.2).label("R34\n10k", loc="bottom")
    d += elm.Ground()
    # output: pull-up, then feedback from a separate column
    d += elm.Line().at(op.out).right(0.6)
    d += (o := elm.Dot())
    d += elm.Resistor().at(o.start).up(2.4).label("R35\n10k")
    d += elm.Vdd().label("+3V3")
    d += elm.Line().at(o.start).right(1.0)
    d += (o2 := elm.Dot())
    d += elm.Line().at(o2.start).up(4.6)
    d += elm.Resistor().left().tox(n2.start[0] + 1.3).label("R36 1M   hysteresis ≈ ±80mV", loc="top")
    d += elm.Line().down().toy(n2.start)
    d += elm.Dot()
    d += elm.Line().at(o2.start).right(0.8)
    d += elm.Arrow().right(0.8).label("TIM input\ncapture", loc="right")
    save(d, "06-mpu-rpm-input")


# ---------------------------------------------------------------- 7. AC voltage sense
def ac_sense():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=1.8)
    d += elm.Line().right(0.001).label("L1 terminal\n240Vrms L-N\n(340Vpk)", loc="left")
    for ref in ["R40", "R41", "R42", "R43"]:
        d += elm.Resistor().right().label(f"{ref}\n330k 1206")
    d += (n1 := elm.Dot())
    d += elm.Resistor().down(2.4).label("R44\n5.62k 1%", loc="bottom")
    d += (vr := elm.Dot())
    d += elm.Line().left(1.2).label("VREF_MID 1.65V\n(buffered)", loc="left")
    d += elm.Diode().at(n1.start).up(1.8).label("D40\nBAV199", loc="bottom")
    d += elm.Vdd().label("+3V3")
    d += elm.Line().at(n1.start).right(1.0)
    d += elm.Resistor().right().label("R45\n1k")
    d += (n2 := elm.Dot())
    d += elm.Capacitor().down(2.4).label("C40 22nF C0G\nfc≈1.1kHz\n(matched V+I)", loc="bottom")
    d += elm.Ground().label("AGND", loc="bottom")
    d += elm.Line().at(n2.start).right(0.8)
    d += elm.Arrow().right(0.8).label("ADC1\n÷237:\n340Vpk→1.43Vpk\naround 1.65V", loc="right")
    save(d, "07-ac-voltage-sense")


# ---------------------------------------------------------------- 8. CT input
def ct_input():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    xf = elm.Transformer(t1=1, t2=4).right().label("CT 200:5 (external,\non load cable)", loc="bottom")
    d += xf
    d += elm.Line().at(xf.p1).left(1.0).label("load cable =\n1-turn primary", loc="left")
    d += elm.Line().at(xf.p2).left(1.0)
    d += elm.Line().at(xf.s1).right(1.4)
    d += (t := elm.Dot())
    d += elm.Line().at(xf.s2).right(1.4)
    d += (b := elm.Dot())
    d += elm.Zener().at((b.start[0] + 1.2, t.start[1])).toy(b.start).label("D50\nSMBJ6.0CA", loc="bottom")
    d += elm.Dot().at((b.start[0] + 1.2, t.start[1]))
    d += elm.Dot().at((b.start[0] + 1.2, b.start[1]))
    d += elm.Resistor().at(t.start).toy(b.start).label("R50\n0.05Ω 3W\n(burden)", loc="top")
    d += elm.Line().at(b.start).right(2.2)
    d += elm.Line().down(1.0).label("VREF_MID\n1.65V", loc="bottom")
    # signal to opamp non-inverting input
    d += elm.Line().at(t.start).right(2.4)
    op = elm.Opamp(leads=True).anchor("in2").label("U4 MCP6002\ngain ×2", loc="bottom")
    d += op
    # feedback network routed above
    d += elm.Line().at(op.in1).up(1.6)
    d += (a := elm.Dot())
    d += elm.Resistor().at(a.start).left(2.4).label("R51 10k", loc="top")
    d += elm.Line().left(0.8).label("VREF_MID", loc="left")
    d += elm.Line().at(a.start).up(1.4)
    d += elm.Resistor().right(2.8).label("R52 10k", loc="top")
    d += (fb2 := elm.Dot())
    d += elm.Line().at(op.out).right(0.6)
    d += (o := elm.Dot())
    d += elm.Line().at(fb2.start).tox(o.start)
    d += elm.Line().toy(o.start)
    d += elm.Resistor().at(o.start).right().label("R53 6.8k\n(phase match)")
    d += (n2 := elm.Dot())
    d += elm.Capacitor().down(2.0).label("C50\n22nF", loc="bottom")
    d += elm.Ground().label("AGND", loc="bottom")
    d += elm.Line().at(n2.start).right(0.6)
    d += elm.Arrow().right(0.8).label("ADC3\n5A→±0.71V\naround 1.65V", loc="right")
    save(d, "08-ct-input")


# ---------------------------------------------------------------- 9. relay driver
def relay_driver():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    q = elm.NFet(bulk=True).at((5.6, 0)).anchor("gate").label("Q60 2N7002K\n60V logic FET", loc="right")
    d += q
    d += elm.Line().at((0, 0)).right(0.001).label("MCU GPIO", loc="left")
    d += elm.Resistor().at((0, 0)).right(2.6).label("R60\n100Ω")
    d += (g := elm.Dot())
    d += elm.Resistor().at(g.start).down(2.2).label("R61\n100k", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(g.start).to(q.gate)
    d += elm.Ground().at(q.source)
    d += elm.Line().at(q.drain).up(0.7)
    d += (nd := elm.Dot())
    d += elm.Inductor2(loops=3).at(nd.start).up(2.4).label("K1 coil\n24V 360Ω", loc="top")
    d += (td := elm.Dot())
    d += elm.Line().up(0.5).label("+24V_SW", loc="top")
    d += elm.Line().at(nd.start).right(2.0)
    d += elm.Diode().up(2.4).label("D60 SS34\nflyback", loc="bottom")
    d += elm.Line().left(2.0)
    # contacts, drawn separately
    d += elm.Line().at((10.5, 1.2)).right(0.001).label("COM", loc="left")
    d += elm.Switch().right().label("K1 contacts 16A", loc="top")
    d += elm.Line().right(0.5).label("NO → fuel solenoid\nterminal", loc="right")
    save(d, "09-relay-driver")


# ---------------------------------------------------------------- 10/11. comms
def comms():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    ic = elm.Ic(
        pins=[
            elm.IcPin(name="TXD", side="left", slot="4/4"),
            elm.IcPin(name="RXD", side="left", slot="3/4"),
            elm.IcPin(name="VIO", side="left", slot="2/4"),
            elm.IcPin(name="VCC", side="left", slot="1/4"),
            elm.IcPin(name="CANH", side="right", slot="3/4"),
            elm.IcPin(name="CANL", side="right", slot="2/4"),
            elm.IcPin(name="GND", side="bottom"),
        ],
        edgepadW=1.4,
        edgepadH=1.0,
        pinspacing=3.0,
        label="U5  TJA1051T/3",
        lblloc="top",
    )
    d += ic
    d += elm.Ground().at(ic.GND)
    d += elm.Line().at(ic.TXD).left(1.0).label("CAN1_TX", loc="left")
    d += elm.Line().at(ic.RXD).left(1.0).label("CAN1_RX", loc="left")
    d += elm.Line().at(ic.VIO).left(1.0).label("+3V3", loc="left")
    d += elm.Line().at(ic.VCC).left(1.0).label("+5V", loc="left")
    d += elm.Line().at(ic.CANH).right(1.4)
    d += (h := elm.Dot())
    d += elm.Line().right(3.6).label("CAN_H →", loc="right")
    d += elm.Line().at(ic.CANL).right(1.4)
    d += (l := elm.Dot())
    d += elm.Line().right(3.6).label("CAN_L →", loc="right")
    d += elm.Resistor().at(h.start).down(1.5).label("R70\n60Ω", loc="bottom")
    d += (m := elm.Dot())
    d += elm.Resistor().down(1.5).label("R71 60Ω\n(JP1: end-\nnode only)", loc="bottom")
    d += elm.Line().at(m.start).right(6.2)
    d += elm.Capacitor().down(1.2).label("C70\n4.7nF", loc="bottom")
    d += elm.Ground()
    save(d, "10-can-transceiver")

    d2 = schemdraw.Drawing()
    d2.config(fontsize=10, unit=2.0)
    ic2 = elm.Ic(
        pins=[
            elm.IcPin(name="D", side="left", slot="4/4"),
            elm.IcPin(name="R", side="left", slot="3/4"),
            elm.IcPin(name="DE/RE", anchorname="DE", side="left", slot="2/4"),
            elm.IcPin(name="VCC", side="left", slot="1/4"),
            elm.IcPin(name="A", side="right", slot="3/4"),
            elm.IcPin(name="B", side="right", slot="2/4"),
            elm.IcPin(name="GND", side="bottom"),
        ],
        edgepadW=1.4,
        edgepadH=1.0,
        pinspacing=3.0,
        label="U6  THVD1450",
        lblloc="top",
    )
    d2 += ic2
    d2 += elm.Ground().at(ic2.GND)
    d2 += elm.Line().at(ic2.D).left(1.0).label("USART_TX", loc="left")
    d2 += elm.Line().at(ic2.R).left(1.0).label("USART_RX", loc="left")
    d2 += elm.Line().at(ic2.DE).left(1.0).label("GPIO dir", loc="left")
    d2 += elm.Line().at(ic2.VCC).left(1.0).label("+3V3", loc="left")
    d2 += elm.Line().at(ic2.A).right(1.4)
    d2 += (a := elm.Dot())
    d2 += elm.Line().right(3.6).label("485_A →", loc="right")
    d2 += elm.Line().at(ic2.B).right(1.4)
    d2 += (b := elm.Dot())
    d2 += elm.Line().right(3.6).label("485_B →", loc="right")
    d2 += elm.Resistor().at(a.start).toy(b.start).label("R72 120Ω\n(JP2 term)", loc="bottom")
    d2 += elm.Resistor().at(a.start).up(2.2).label("R73 560Ω\nfail-safe", loc="top")
    d2 += elm.Vdd().label("+3V3")
    d2 += elm.Resistor().at(b.start).down(1.8).label("R74 560Ω\nfail-safe", loc="bottom")
    d2 += elm.Ground()
    save(d2, "11-rs485-transceiver")


# ---------------------------------------------------------------- 00a. system context
def system_context():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    mains = flow.Box(w=4.2, h=1.4).at((0, 9)).label("UTILITY MAINS\n415V 3-ph")
    d += mains
    km = flow.Box(w=3.0, h=1.2).at((6.6, 9.1)).label("KM mains\ncontactor")
    d += km
    load = flow.Box(w=3.4, h=1.4).at((13.0, 9)).label("LOAD\n(building)")
    d += load
    gen = flow.Box(w=5.2, h=2.4).at((-0.5, 5.6)).label(
        "GENSET\nengine (common-rail)\n+ ENGINE ECU\n+ 415V alternator"
    )
    d += gen
    kg = flow.Box(w=3.0, h=1.2).at((6.6, 5.2)).label("KG gen\ncontactor")
    d += kg
    ecu = flow.Box(w=4.4, h=2.0).at((6.0, 1.0)).label("ECU-25\ngenset supervisor\n(this project)")
    d += ecu
    disp = flow.Box(w=3.0, h=1.2).at((0, -0.8)).label("DISPLAY\nboard")
    d += disp
    scada = flow.Box(w=3.4, h=1.2).at((13.2, 1.0)).label("SCADA / BMS\n/ laptop")
    d += scada
    batt = flow.Box(w=2.6, h=1.1).at((13.6, 3.7)).label("24V\nbattery")
    d += batt
    # power flow
    d += elm.Arrow().at(mains.E).to(km.W)
    d += elm.Arrow().at(km.E).to(load.W).label("415V", loc="top")
    d += elm.Arrow().at(gen.E).toy(kg.W).tox(kg.W)
    d += elm.Line().at(kg.E).tox(load.S)
    d += elm.Arrow().toy(load.S)
    # control
    d += elm.Arrow().at(ecu.N).toy(kg.S)
    d += elm.Line().at((6.3, 2.0)).toy(km.S[1])
    d += elm.Arrow().tox(km.S)
    d += elm.Label().at((ecu.N[0] - 2.6, 3.6)).label("K3/K4 coils\n+ interlock")
    d += elm.Line().at(ecu.W).tox(gen.S)
    d += elm.Arrow().toy(gen.S).label("J1939 CAN + run/start\n+ legacy senders", loc="bot")
    d += elm.Line().at(ecu.S).toy(disp.E)
    d += elm.Arrow().tox(disp.E).label("ribbon SPI", loc="bot")
    d += elm.Arrow().at(ecu.E).tox(scada.W).label("RS485 Modbus", loc="top")
    d += elm.Line().at(batt.S).toy(ecu.E[1] + 0.9)
    d += elm.Arrow().tox(ecu.E).label("24V DC", loc="top")
    save(d, "00a-system-context")


# ---------------------------------------------------------------- 00b. main board subsystems
def board_subsystems():
    d = schemdraw.Drawing()
    d.config(fontsize=9, unit=2.0)
    ins = [
        ("8× DIGITAL IN\ndivider+clamp+RC", 9.0),
        ("3× SENDER IN\ncurrent source", 7.2),
        ("MPU RPM\ncomparator", 5.4),
        ("6× AC SENSE\n1.3MΩ dividers", 3.6),
        ("3× CT IN\nburden+amp", 1.8),
    ]
    inboxes = []
    for label, y in ins:
        b = flow.Box(w=3.6, h=1.3).at((0, y)).label(label)
        d += b
        inboxes.append(b)
    psu = flow.Box(w=4.4, h=1.3).at((6.2, 12.0)).label("PSU: protect → buck\n5V → LDO 3.3V")
    d += psu
    mcu = flow.Box(w=4.4, h=8.6).at((6.2, 5.8)).label(
        "STM32F407VGT6\n\nengine_fsm\namf_fsm\nprotection\nmetering\nhmi · comms\nconfig"
    )
    d += mcu
    outs = [
        ("6× RELAYS\nrun/start/contactors\nhorn/glow", 9.2),
        ("CAN TJA1051\nJ1939 → engine ECU", 7.2),
        ("RS485 THVD1450\nModbus slave", 5.4),
        ("RIBBON → display\nSPI + strobes", 3.6),
        ("EEPROM M95M02\nconfig + fault log", 1.8),
    ]
    outboxes = []
    for label, y in outs:
        b = flow.Box(w=4.0, h=1.4).at((13.2, y)).label(label)
        d += b
        outboxes.append(b)
    for b in inboxes:
        d += elm.Arrow().at(b.E).tox(mcu.W)
    for i, b in enumerate(outboxes):
        d += elm.Line(arrow="<->" if i in (1, 2, 4) else "->").at((mcu.E[0], b.W[1])).tox(b.W)
    d += elm.Arrow().at(psu.S).to(mcu.N)
    d += elm.Arrow().at((0.4, 12.0)).tox(psu.W).label("24V battery in", loc="top")
    save(d, "00b-main-board-subsystems")


# ---------------------------------------------------------------- 12. MCU core
def mcu_core():
    d = schemdraw.Drawing()
    d.config(fontsize=9, unit=2.0)
    ic = elm.Ic(
        pins=[
            elm.IcPin(name="VDD", side="left", slot="6/6"),
            elm.IcPin(name="VCAP", side="left", slot="5/6"),
            elm.IcPin(name="VDDA", side="left", slot="4/6"),
            elm.IcPin(name="VBAT", side="left", slot="3/6"),
            elm.IcPin(name="NRST", side="left", slot="2/6"),
            elm.IcPin(name="BOOT0", side="left", slot="1/6"),
            elm.IcPin(name="OSC_IN", side="right", slot="6/6"),
            elm.IcPin(name="OSC_OUT", side="right", slot="5/6"),
            elm.IcPin(name="OSC32_IN", side="right", slot="4/6"),
            elm.IcPin(name="OSC32_OUT", side="right", slot="3/6"),
            elm.IcPin(name="PA13", side="right", slot="2/6"),
            elm.IcPin(name="PA14", side="right", slot="1/6"),
            elm.IcPin(name="VSS", side="bottom"),
        ],
        edgepadW=1.6,
        edgepadH=1.0,
        pinspacing=2.6,
        label="U7  STM32F407VGT6",
        lblloc="top",
    )
    d += ic
    d += elm.Ground().at(ic.VSS)
    # VDD: rail symbol + decoupling (horizontal cap)
    d += elm.Line().at(ic.VDD).left(1.2)
    d += (vdd := elm.Dot())
    d += elm.Vdd().at(vdd.start).label("+3V3")
    d += elm.Capacitor().at(vdd.start).left(2.2).label("100nF ×11 + 4.7µF", loc="top")
    d += elm.Ground()
    # VCAP core-regulator caps (mandatory)
    d += elm.Line().at(ic.VCAP).left(1.2)
    d += (vcap := elm.Dot())
    d += elm.Capacitor().at(vcap.start).down(1.5).label("C78,C79 2×2.2µF\n(VCAP1, VCAP2)", loc="bottom")
    d += elm.Ground()
    # VDDA: ferrite from 3V3 + local cap
    d += elm.Line().at(ic.VDDA).left(1.2)
    d += (vda := elm.Dot())
    d += elm.Inductor2(loops=2).at(vda.start).left(2.2).label("FB2 ferrite", loc="top")
    d += elm.Vdd().label("+3V3")
    d += elm.Capacitor().at(vda.start).down(1.5).label("1µF+10nF\nVREF+ → VDDA", loc="bottom")
    d += elm.Ground()
    # VBAT: BAT54 charge path + supercap
    d += elm.Line().at(ic.VBAT).left(1.2)
    d += (vb := elm.Dot())
    d += elm.Vdd().at((vb.start[0] - 5.0, vb.start[1])).label("+3V3")
    d += elm.Diode().at((vb.start[0] - 5.0, vb.start[1])).right(2.0).label("D70 BAT54", loc="top")
    d += elm.Resistor().right().tox(vb.start).label("R81 330Ω", loc="bottom")
    d += elm.Capacitor(polar=True).at(vb.start).down(1.5).label("C70 0.22F\nsupercap", loc="bottom")
    d += elm.Ground()
    # NRST: cap + test point
    d += elm.Line().at(ic.NRST).left(1.2)
    d += (nr := elm.Dot())
    d += elm.Capacitor().at(nr.start).down(1.5).label("C71\n100nF", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(nr.start).left(2.2).label("TP1 / SWD pin 10", loc="left")
    # BOOT0: pulldown + jumper
    d += elm.Line().at(ic.BOOT0).left(1.2)
    d += (bt := elm.Dot())
    d += elm.Resistor().at(bt.start).down(1.8).label("R80\n10k", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(bt.start).left(2.2).label("JP3 → 3V3 =\nROM bootloader", loc="left")
    # 8 MHz crystal, caps horizontal outward
    d += elm.Line().at(ic.OSC_IN).right(1.6)
    d += (x1 := elm.Dot())
    d += elm.Line().at(ic.OSC_OUT).right(1.6)
    d += (x2 := elm.Dot())
    d += elm.Crystal().at(x1.start).toy(x2.start).label("Y1\n8MHz", loc="bottom")
    d += elm.Capacitor().at(x1.start).right(2.2).label("C72 12pF", loc="top")
    d += elm.Ground()
    d += elm.Capacitor().at(x2.start).right(2.2).label("C73 12pF", loc="top")
    d += elm.Ground()
    # 32.768 kHz crystal
    d += elm.Line().at(ic.OSC32_IN).right(1.6)
    d += (x3 := elm.Dot())
    d += elm.Line().at(ic.OSC32_OUT).right(1.6)
    d += (x4 := elm.Dot())
    d += elm.Crystal().at(x3.start).toy(x4.start).label("Y2\n32.768kHz", loc="bottom")
    d += elm.Capacitor().at(x3.start).right(2.2).label("C74 6.8pF", loc="top")
    d += elm.Ground()
    d += elm.Capacitor().at(x4.start).right(2.2).label("C75 6.8pF", loc="top")
    d += elm.Ground()
    # SWD
    d += elm.Line().at(ic.PA13).right(1.6).label("SWDIO → J2 SWD pin 2", loc="right")
    d += elm.Line().at(ic.PA14).right(1.6).label("SWCLK → J2 SWD pin 4", loc="right")
    save(d, "12-mcu-core")


# ---------------------------------------------------------------- 13. VREF buffer
def vref_buffer():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    d += elm.Vdd().at((0, 4.4)).label("+3V3")
    d += elm.Resistor().at((0, 4.4)).down(2.2).label("R90\n10k 1%")
    d += (mid := elm.Dot())
    d += elm.Resistor().down(2.2).label("R91\n10k 1%", loc="bottom")
    d += elm.Ground()
    d += elm.Capacitor().at(mid.start).left(2.0).label("C90\n100nF", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(mid.start).right(1.2)
    op = elm.Opamp(leads=True).anchor("in2").label("U8B MCP6002\nunity buffer", loc="bottom")
    d += op
    d += elm.Line().at(op.out).right(0.6)
    d += (o := elm.Dot())
    d += elm.Resistor().at(o.start).right().label("R92\n47Ω")
    d += (vr := elm.Dot())
    d += elm.Capacitor().down(1.8).label("C91 10µF\n(dual-feedback RC\nin KiCad)", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(op.in1).up(2.2)
    d += elm.Line().tox(vr.start)
    d += elm.Line().toy(vr.start)
    d += elm.Dot()
    d += elm.Line().at(vr.start).right(0.8)
    d += elm.Arrow().right(0.8).label("VREF_MID 1.65V\nbias for 6× AC + 3× CT", loc="right")
    save(d, "13-vref-mid-buffer")


# ---------------------------------------------------------------- 14. switched 24V rail
def sw24():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.2)
    d += elm.Line().right(0.001).label("+24V_PROT", loc="left")
    d += elm.Fuse().right().label("F2 PTC 1.1A\n(resettable)", loc="top")
    d += (n := elm.Dot())
    d += elm.Zener().down().reverse().label("D80\nSMBJ33A", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n.start).right(1.2)
    d += elm.Arrow().right(0.8).label("+24V_SW\nrelay coils K1–K6\n+ D+ excitation", loc="right")
    save(d, "14-24v-switched-rail")


# ---------------------------------------------------------------- 15. battery sense
def battery_sense():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    d += elm.Line().right(0.001).label("+24V_PROT\n(= battery, fused)", loc="left")
    d += elm.Resistor().right().label("R85\n100k 1%")
    d += (n1 := elm.Dot())
    d += elm.Resistor().at(n1.start).down(2.2).label("R86\n6.8k 1%", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n1.start).right(0.8)
    d += elm.Resistor().right().label("R87\n4.7k")
    d += (n2 := elm.Dot())
    d += elm.Diode().at(n2.start).up(1.7).label("D81\nBAV199", loc="bottom")
    d += elm.Vdd().label("+3V3")
    d += elm.Capacitor().at(n2.start).down(1.7).label("C85\n100nF", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n2.start).right(0.8)
    d += elm.Arrow().right(0.8).label("ADC\n÷15.7:\n32V → 2.04V", loc="right")
    save(d, "15-battery-voltage-sense")


# ---------------------------------------------------------------- 16. charge alternator D+
def dplus():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    d += elm.Line().right(0.001).label("+24V_SW\n(on with run enable)", loc="left")
    d += elm.Resistor().right().label("R88 220Ω 5W\n(excitation ≈100mA)", loc="top")
    d += (dp := elm.Dot())
    d += elm.Line().at(dp.start).down(1.6).label("D+ terminal →\ncharge alternator", loc="bottom")
    d += elm.Line().at(dp.start).right(1.0)
    d += elm.Resistor().right().label("R85a\n100k 1%")
    d += (n1 := elm.Dot())
    d += elm.Resistor().at(n1.start).down(2.0).label("R86a\n6.8k 1%", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n1.start).right(0.8)
    d += elm.Resistor().right().label("R87a\n4.7k")
    d += (n2 := elm.Dot())
    d += elm.Diode().at(n2.start).up(1.7).label("D82\nBAV199", loc="bottom")
    d += elm.Vdd().label("+3V3")
    d += elm.Capacitor().at(n2.start).down(1.7).label("C86\n100nF", loc="bottom")
    d += elm.Ground()
    d += elm.Line().at(n2.start).right(0.8)
    d += elm.Arrow().right(0.8).label("ADC\nengine runs +\nD+ low = charge fail", loc="right")
    save(d, "16-charge-alt-dplus")


# ---------------------------------------------------------------- 17. EEPROM
def eeprom():
    d = schemdraw.Drawing()
    d.config(fontsize=10, unit=2.0)
    ic = elm.Ic(
        pins=[
            elm.IcPin(name="CS", side="left", slot="4/4"),
            elm.IcPin(name="SCK", side="left", slot="3/4"),
            elm.IcPin(name="MOSI", side="left", slot="2/4"),
            elm.IcPin(name="MISO", side="left", slot="1/4"),
            elm.IcPin(name="WP", side="right", slot="3/4"),
            elm.IcPin(name="HOLD", side="right", slot="2/4"),
            elm.IcPin(name="VCC", side="top"),
            elm.IcPin(name="VSS", side="bottom"),
        ],
        edgepadW=1.6,
        edgepadH=0.8,
        pinspacing=1.8,
    )
    d += ic
    d += elm.Ground().at(ic.VSS)
    d += elm.Label().at((ic.VCC[0] + 3.0, ic.VCC[1] + 1.4)).label("U9  M95M02-DR")
    d += elm.Line().at(ic.CS).left(1.2)
    d += (cs := elm.Dot())
    d += elm.Resistor().at(cs.start).up(1.8).label("R95\n10k")
    d += elm.Vdd().label("+3V3")
    d += elm.Line().at(cs.start).left(1.2).label("SPI2_NSS (PB12)", loc="left")
    d += elm.Line().at(ic.SCK).left(2.4).label("SPI2_SCK (PB13)", loc="left")
    d += elm.Line().at(ic.MOSI).left(2.4).label("SPI2_MOSI (PB15)", loc="left")
    d += elm.Line().at(ic.MISO).left(2.4).label("SPI2_MISO (PB14)", loc="left")
    d += elm.Line().at(ic.WP).right(0.6)
    d += elm.Resistor().right(1.8).label("R96 10k", loc="top")
    d += elm.Vdd().label("+3V3")
    d += elm.Line().at(ic.HOLD).right(0.6)
    d += elm.Resistor().right(1.8).label("R97 10k", loc="top")
    d += elm.Vdd().label("+3V3")
    d += elm.Line().at(ic.VCC).up(1.0)
    d += (vc := elm.Dot())
    d += elm.Vdd().at(vc.start).label("+3V3")
    d += elm.Capacitor().at(vc.start).left(2.2).label("C95 100nF", loc="top")
    d += elm.Ground()
    save(d, "17-eeprom-spi")


# ---------------------------------------------------------------- 18. display board
def display_board():
    d = schemdraw.Drawing()
    d.config(fontsize=9, unit=2.0)
    j = flow.Box(w=2.6, h=5.2).at((0, 2.0)).label("J3\nribbon\n16-way\n\n3V3 · 5V\nGND\nSPI\nstrobes")
    d += j
    lcd = flow.Box(w=4.6, h=1.6).at((6.0, 6.4)).label("LCD 128×64\nST7565  (SPI)\nCS · A0 · RST + bias caps")
    d += lcd
    sr_o = flow.Box(w=4.6, h=1.6).at((6.0, 3.6)).label("74HC595\nshift reg OUT\n→ 8 LEDs via 470Ω")
    d += sr_o
    sr_i = flow.Box(w=4.6, h=1.6).at((6.0, 0.8)).label("74HC165\nshift reg IN\n← 7 keys + 10k pull-ups")
    d += sr_i
    leds = flow.Box(w=2.6, h=1.4).at((12.4, 3.7)).label("8× LED\nstatus")
    d += leds
    keys = flow.Box(w=2.6, h=1.4).at((12.4, 0.9)).label("7× keys\nSTOP AUTO\nMAN START\n▲ ▼ ⏎")
    d += keys
    d += elm.Line().at((j.E[0] - 0.01, j.NE[1] - 0.4)).right(0.9)
    d += elm.Line().toy(lcd.W)
    d += elm.Arrow().tox(lcd.W).label("SCK/MOSI + LCD_CS/A0/RST", loc="top")
    d += elm.Arrow().at((j.E[0], sr_o.W[1])).tox(sr_o.W).label("SCK/MOSI + RCLK", loc="top")
    d += elm.Line().at((j.E[0], sr_i.W[1])).tox(sr_i.W)
    d += elm.Arrow().at(sr_i.W).tox(j.E[0]).label("MISO ← + LD/CLK", loc="bottom")
    d += elm.Arrow().at(sr_o.E).to(leds.W)
    d += elm.Arrow().at(keys.W).to(sr_i.E)
    save(d, "18-display-board")


if __name__ == "__main__":
    input_protection()
    buck()
    ldo()
    digital_input()
    sender_input()
    mpu()
    ac_sense()
    ct_input()
    relay_driver()
    comms()
    system_context()
    board_subsystems()
    mcu_core()
    vref_buffer()
    sw24()
    battery_sense()
    dplus()
    eeprom()
    display_board()
    print("done ->", os.path.abspath(OUT))
