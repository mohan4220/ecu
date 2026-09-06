"""
gen_ecu25.py — generates the ECU-25 main-board KiCad project.

Source of truth for values: docs/circuits/README.md (independently reviewed).
Regenerate:  .venv/bin/python hardware/kicad/gen/gen_ecu25.py
Output:      hardware/kicad/ecu25-main/
"""

import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kisch import Project, snap

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ecu25-main")

p = Project("ecu25-main", "ECU-25 genset controller — main board")

# ----------------------------------------------------------------- helpers

def R(s, ref, val, x, y, rot=0):
    return s.place("Device:R", ref, val, (x, y), rot)

def C(s, ref, val, x, y, rot=0):
    return s.place("Device:C", ref, val, (x, y), rot)

def CP(s, ref, val, x, y, rot=0):
    return s.place("Device:C_Polarized", ref, val, (x, y), rot)

def gnd(s, at):
    s.power("GND", at)

def rail(s, net, at):
    s.power(net, at)

def clamp99(s, ref, node, val="BAV199"):
    """Dual series diode clamp: node -> +3V3 (top), GND -> node (bottom).
    BAV99 symbol chain: pin1(A) -> pin3(center) -> pin2(K). Rot 90 puts
    pin1 bottom, pin2 top, pin3 right; place so pin3 lands on the node."""
    d = s.place("Diode:BAV99", ref, val, (node[0] - 5.08, node[1]), rot=90,
                ref_at=(node[0] - 9.5, node[1] - 1.3),
                val_at=(node[0] - 9.5, node[1] + 1.3))
    rail(s, "+3V3", d.pin(2))
    gnd(s, d.pin(1))
    return d

def conn(s, ref, val, npins, x, y, name=""):
    """1-row terminal on the left edge, pins pointing right (mirror y)."""
    c = s.place(f"Connector_Generic:Conn_01x{npins:02d}", ref, val, (x, y),
                mirror="y", ref_at=(x - 2, y - 4.2), val_at=(x - 2, y - 6.8))
    return c

# =================================================================
# Sheet 1 — POWER
# =================================================================
s = p.new_sheet("Power", "power.kicad_sch")
s.text((20, 18), "12V system. Input protection + 5V buck + 3.3V LDO + switched"
                 " 12V rail.  '+12V' = protected rail (post Q1); '+12V_HLD' is"
                 " the diode-isolated hold-up node that rides out crank dips.", size=2.0)

# --- input protection chain, left to right at y=50
y = 50.0
j1 = conn(s, "J1", "Screw_Terminal_BATT", 2, 25, y)
s.label(j1.pin(1), "VBAT_IN")
gnd(s, j1.pin(2))

f1 = s.place("Device:Fuse", "F1", "5A blade", (45, y), rot=90)
s.wire((40, y), f1.pin(1))
s.label((40, y), "VBAT_IN")
# D_TVS rot 90: pin2 top, pin1 bottom — rail joins the TOP pin only
d1 = s.place("Device:D_TVS", "D1", "SMCJ16CA", (55, y + 10), rot=90)
s.wire(f1.pin(2), (d1.pin(2)[0], y))          # to TVS column
s.junction((d1.pin(2)[0], y))
s.wire((d1.pin(2)[0], y), d1.pin(2))
gnd(s, d1.pin(1))
# reverse-polarity P-FET: drain to battery side, source to load.
# GDS variant matches DPAK pad numbering (1=G, 2=D tab, 3=S) — the GSD
# variant on a DPAK/PowerPAK footprint silently swaps D and S in the netlist.
q1 = s.place("Device:Q_PMOS_GDS", "Q1", "SQD50P06-15L", (70, y - 2.54), rot=90,
             ref_at=(66, y - 9), val_at=(66, y - 6.5))
# rot 90: D(2.54,5.08)->(-5.08,y-2.54-2.54)... use computed pins
dpin, spin, gpin = q1.pin(2), q1.pin(3), q1.pin(1)
s.wire((d1.pin(2)[0], y), (dpin[0], y), dpin)
s.wire(spin, (spin[0] + 6, spin[1] if abs(spin[1]-y) < 3 else y))
# gate network
r1 = R(s, "R1", "100k", gpin[0], gpin[1] + 12)
s.wire(gpin, r1.pin(1))
gnd(s, r1.pin(2))
d2 = s.place("Device:D_Zener", "D2", "BZT52C12", (gpin[0] + 7, gpin[1] + 5), rot=0,
             ref_at=(gpin[0] + 9.5, gpin[1] + 3), val_at=(gpin[0] + 9.5, gpin[1] + 5.5))
s.wire(gpin, (gpin[0], d2.pin(2)[1]) if False else gpin)  # keep simple below
s.wire(d2.pin(2), (d2.pin(2)[0], gpin[1]))                 # zener A up to gate row? see below
# D2 anode -> gate node, cathode -> source (holds VGS <= 12V)
s.wire((d2.pin(2)[0], gpin[1]), gpin)
s.junction(gpin)
s.wire(d2.pin(1), (spin[0], d2.pin(1)[1]), spin)
s.junction(spin)

fb1 = s.place("Device:FerriteBead", "FB1", "600R@100MHz 3A", (95, y), rot=90,
              ref_at=(92, y - 6.5), val_at=(90, y - 4))
sx = spin[0] + 6
s.wire((sx, y), fb1.pin(1))
c1 = CP(s, "C1", "100uF 35V", 105, y + 10)
c2 = C(s, "C2", "100nF", 120, y + 10)
s.wire(fb1.pin(2), (c1.pin(1)[0], y))
s.junction((c1.pin(1)[0], y))
s.wire((c1.pin(1)[0], y), c1.pin(1))
s.wire((c1.pin(1)[0], y), (c2.pin(1)[0], y))
s.junction((c2.pin(1)[0], y))
s.wire((c2.pin(1)[0], y), c2.pin(1))
gnd(s, c1.pin(2)); gnd(s, c2.pin(2))
s.wire((c2.pin(1)[0], y), (125, y))
rail(s, "+12V", (125, y))

# --- buck at y=110
u1 = s.place("Regulator_Switching:LM5164DDA", "U1", "LM5164DDA-Q1", (110, 110),
             ref_at=(104, 94), val_at=(112, 94))
vin = u1.pin(2)
# +12V -> D3 (blocking) -> C9 hold-up -> VIN. The diode stops the bank
# back-feeding the harness during a crank dip, so the logic rides through;
# the coils tap +12V ahead of it and simply latch through the sag.
d3 = s.place("Device:D_Schottky", "D3", "SS34", (vin[0] - 24, vin[1]), rot=180,
             ref_at=(vin[0] - 27, vin[1] - 5), val_at=(vin[0] - 27, vin[1] - 2.5))
# rot 180 puts pin1 (K) on the RIGHT and pin2 (A) on the LEFT. Anode faces
# the battery, cathode feeds the buck — wiring a pin back across the body
# shorts the diode out through the other pin once wires are normalized.
kk3, aa3 = d3.pin(1), d3.pin(2)
if not kk3[0] > aa3[0]:
    raise SystemExit("D3: rot=180 no longer puts K right of A")
s.wire(aa3, (vin[0] - 34, vin[1]))
rail(s, "+12V", (vin[0] - 34, vin[1]))
s.wire(kk3, (vin[0] - 8, vin[1]))
s.junction((vin[0] - 8, vin[1]))
s.wire((vin[0] - 8, vin[1]), vin)
c9 = CP(s, "C9", "2200uF 25V", vin[0] - 16, vin[1] + 10)
s.wire((vin[0] - 16, vin[1]), c9.pin(1))
s.junction((vin[0] - 16, vin[1]))
gnd(s, c9.pin(2))
c3 = C(s, "C3", "2.2uF 50V", vin[0] - 8, vin[1] + 10)
s.wire((vin[0] - 8, vin[1]), c3.pin(1))
gnd(s, c3.pin(2))
# UVLO divider
en = u1.pin(3)
r2 = R(s, "R2", "100k", en[0] - 15, en[1] - 6)
r3 = R(s, "R3", "30.1k 1% (UVLO 6.5V)", en[0] - 15, en[1] + 6)
s.wire(r2.pin(2), r3.pin(1))
# single run EN -> divider midpoint (a stray stub here used to end ON C3's
# column and shorted EN to +12V — reviewer finding M1)
s.wire_h_then_v(en, ((r2.pin(2)[0] + r3.pin(1)[0]) / 2, (r2.pin(2)[1] + r3.pin(1)[1]) / 2))
s.junction(((r2.pin(2)[0] + r3.pin(1)[0]) / 2, (r2.pin(2)[1] + r3.pin(1)[1]) / 2))
# UVLO senses the HOLD-UP node, not the battery: sensing +12V would trip
# EN the moment the harness sags during crank and switch the buck off while
# C9 still holds plenty of charge, defeating the whole ride-through.
s.wire(r2.pin(1), (r2.pin(1)[0], vin[1]))
s.junction((r2.pin(1)[0], vin[1]))
gnd(s, r3.pin(2))
# RON — R4 on its own column; at ron-8 it shared C3's column and the
# vertical run passed through C3's grounded pin (reviewer finding C1)
ron = u1.pin(4)
r4 = R(s, "R4", "49.9k 1% (RON 300kHz)", ron[0] - 4, ron[1] + 8)
s.wire_h_then_v(ron, r4.pin(1))
gnd(s, r4.pin(2))
# GND + EP
gnd(s, u1.pin(1))
s.wire(u1.pin(9), (u1.pin(9)[0], u1.pin(9)[1] + 2.54), (u1.pin(1)[0], u1.pin(1)[1] + 2.54), u1.pin(1))
# BST-SW cap, inductor, output
bst, sw = u1.pin(7), u1.pin(8)
c4 = C(s, "C4", "100nF", bst[0] + 8, (bst[1] + sw[1]) / 2, rot=0)
s.wire(bst, (c4.pin(1)[0], bst[1]), c4.pin(1))
s.wire(sw, (c4.pin(2)[0], sw[1]))
s.junction((c4.pin(2)[0], sw[1]))
s.wire(c4.pin(2), (c4.pin(2)[0], sw[1]))
l2 = s.place("Device:L", "L2", "33uH 2A shielded", (c4.pin(2)[0] + 16, sw[1]), rot=270,
             ref_at=(c4.pin(2)[0] + 14, sw[1] - 7), val_at=(c4.pin(2)[0] + 9, sw[1] - 4.5))
s.wire((c4.pin(2)[0], sw[1]), l2.pin(2) if l2.pin(2)[0] < l2.pin(1)[0] else l2.pin(1))
lo = l2.pin(1) if l2.pin(2)[0] < l2.pin(1)[0] else l2.pin(2)
c5 = C(s, "C5", "22uF 25V", lo[0] + 6, sw[1] + 10)
c6 = C(s, "C6", "22uF 25V", lo[0] + 14, sw[1] + 10)
s.wire(lo, (c5.pin(1)[0], sw[1]))
s.junction((c5.pin(1)[0], sw[1]))
s.wire((c5.pin(1)[0], sw[1]), c5.pin(1))
s.wire((c5.pin(1)[0], sw[1]), (c6.pin(1)[0], sw[1]))
s.junction((c6.pin(1)[0], sw[1]))
s.wire((c6.pin(1)[0], sw[1]), c6.pin(1))
gnd(s, c5.pin(2)); gnd(s, c6.pin(2))
# FB divider from output
r5 = R(s, "R5", "38.3k 1%", lo[0] + 22, sw[1] + 8)
r6 = R(s, "R6", "12.1k 1%", lo[0] + 22, sw[1] + 20)
s.wire((c6.pin(1)[0], sw[1]), (r5.pin(1)[0], sw[1]), r5.pin(1))
s.junction((r5.pin(1)[0], sw[1]))
s.wire(r5.pin(2), r6.pin(1))
fbm = ((r5.pin(2)[1] + r6.pin(1)[1]) / 2)
fb = u1.pin(5)
s.wire((r5.pin(2)[0], fbm), (r5.pin(2)[0] + 6, fbm))
s.junction((r5.pin(2)[0], fbm))
s.label((r5.pin(2)[0] + 6, fbm), "BUCK_FB")
s.wire(fb, (fb[0] + 4, fb[1]))
s.label((fb[0] + 4, fb[1]), "BUCK_FB")
gnd(s, r6.pin(2))
s.no_connect(u1.pin(6))  # PGOOD unused
s.wire((r5.pin(1)[0], sw[1]), (r5.pin(1)[0] + 10, sw[1]))
rail(s, "+5V", (r5.pin(1)[0] + 10, sw[1]))

# --- LDO at y=170
u2 = s.place("Regulator_Linear:TLV75533PDBV", "U2", "TLV75533 (500mA)", (110, 170),
             ref_at=(106, 160), val_at=(112, 160))
iin, en2, out = u2.pin(1), u2.pin(3), u2.pin(5)
s.wire(iin, (iin[0] - 8, iin[1]))
rail(s, "+5V", (iin[0] - 8, iin[1]))
s.junction((iin[0] - 8, iin[1]))
c7 = C(s, "C7", "2.2uF", iin[0] - 8, iin[1] + 10)
s.wire((iin[0] - 8, iin[1]), c7.pin(1))
gnd(s, c7.pin(2))
s.wire(en2, (iin[0] - 4, en2[1]), (iin[0] - 4, iin[1]), (iin[0] - 8, iin[1]))
gnd(s, u2.pin(2))
s.no_connect(u2.pin(4))
c8 = C(s, "C8", "10uF", out[0] + 8, out[1] + 10)
s.wire(out, (c8.pin(1)[0], out[1]))
s.junction((c8.pin(1)[0], out[1]))
s.wire((c8.pin(1)[0], out[1]), c8.pin(1))
gnd(s, c8.pin(2))
s.wire((c8.pin(1)[0], out[1]), (c8.pin(1)[0] + 8, out[1]))
rail(s, "+3V3", (c8.pin(1)[0] + 8, out[1]))

# --- +12V_SW at y=220
y = 220.0
# F2 must stand off the ~53V load-dump clamp while tripped into a stuck coil.
# No chip PPTC does that at 1.1A hold (the 1206 0ZCJ0110 is 8V, the 1812 33V
# parts are also short) — so this is a radial-leaded RXEF110, 72V (BOM review).
f2 = s.place("Device:Polyfuse", "F2", "PTC 1.1A 72V", (60, y), rot=90,
             ref_at=(57, y - 6.5), val_at=(57, y - 4))
rail(s, "+12V", (f2.pin(1)[0] - 6, y))
s.wire((f2.pin(1)[0] - 6, y), f2.pin(1))
d80 = s.place("Device:D_TVS", "D80", "SMBJ16CA", (f2.pin(2)[0] + 8, y + 10), rot=90)
s.wire(f2.pin(2), (d80.pin(2)[0], y))
s.junction((d80.pin(2)[0], y))
s.wire((d80.pin(2)[0], y), d80.pin(2))
gnd(s, d80.pin(1))
s.wire((d80.pin(2)[0], y), (d80.pin(2)[0] + 12, y))
s.glabel((d80.pin(2)[0] + 12, y), "+12V_SW", shape="output")
s.text((40, 235), "+12V_SW feeds ONLY the K1-K6 coils (6 x 33mA) and D+ excitation"
                  " (100mA) = 300mA. Field loads are fed from J8 pin 1 by the"
                  " installer, so no field current crosses this board.", size=1.5)

# =================================================================
# Sheet 2 — ANALOG INPUTS (senders, battery sense, D+)
# =================================================================
s = p.new_sheet("Analog inputs", "analog.kicad_sch")
s.text((20, 18), "3x precision current-source sender inputs (LM358+BC857), "
                 "battery voltage sense, charge-alternator D+ excite/sense.", size=2.0)

# 4.5V reference for high-side current sources (0.5V below +5V)
rx, ry = 40.0, 40.0
r70 = R(s, "R70", "10k 1%", rx, ry)
r71 = R(s, "R71", "90.9k 1%", rx, ry + 12)
rail(s, "+5V", r70.pin(1))
s.wire(r70.pin(2), r71.pin(1))
gnd(s, r71.pin(2))
m = ((r70.pin(2)[1] + r71.pin(1)[1]) / 2)
c60 = C(s, "C60", "100nF", rx + 10, m + 8)
s.wire((rx, m), (c60.pin(1)[0], m))
s.junction((rx, m))
s.wire((c60.pin(1)[0], m), c60.pin(1))
gnd(s, c60.pin(2))
s.wire((c60.pin(1)[0], m), (rx + 18, m))
s.label((rx + 18, m), "VREF_4V5")
s.text((rx - 15, ry - 8), "4.5V ref: 0.5V across R_set -> 8.06mA (62R) / 2.0mA (249R)", size=1.5)

j3 = conn(s, "J3", "Screw_Terminal_SENDERS", 4, 25, 100)
s.text((18, 88), "J3: 1=OIL 2=FUEL 3=TEMP 4=GND (sender return)", size=1.5)
gnd(s, j3.pin(4))

CHANNELS = [
    ("OIL",  "62R 1%",  "U3", 1, "R20", "R23", "C20", "R26", "D20", "ADC_OIL",  70.0),
    ("FUEL", "62R 1%",  "U3", 2, "R21", "R24", "C21", "R27", "D21", "ADC_FUEL", 130.0),
    ("TEMP", "249R 1%", "U4", 1, "R22", "R25", "C22", "R28", "D22", "ADC_TEMP", 190.0),
]
for name, rset_v, uref, unit, rset_ref, rf_ref, cf_ref, rs_ref, d_ref, adc, yc in CHANNELS:
    x0 = 75.0
    # high-side source: R_set from +5V to PNP emitter; opamp forces 0.5V across it
    rs_ = R(s, rset_ref, rset_v, x0, yc - 18)
    rail(s, "+5V", rs_.pin(1))
    q = s.place("Transistor_BJT:BC857", f"Q{2+unit if uref=='U3' else 5}", "BC857",
                (x0 - 2.54, yc), rot=0, mirror="x",
                ref_at=(x0 + 3, yc - 2), val_at=(x0 + 3, yc + 0.6))
    # mirror x flips vertically: E top, C bottom
    e_pin = q.pin(2); c_pin = q.pin(3); b_pin = q.pin(1)
    s.wire(rs_.pin(2), e_pin)
    op = s.place("Amplifier_Operational:LM358", uref, "LM358", (x0 - 25, yc + 2.54),
                 unit=unit, ref_at=(x0 - 28, yc + 8), val_at=(x0 - 22, yc + 8))
    # +in = VREF_4V5, -in = emitter (feedback), out -> base via 1k
    plus5 = op.pin("3" if unit == 1 else "5")
    s.wire(plus5, (plus5[0] - 5, plus5[1]))
    s.label((plus5[0] - 5, plus5[1]), "VREF_4V5", rot=180)
    inv = op.pin("2" if unit == 1 else "6")
    s.wire(inv, (inv[0] - 3, inv[1]), (inv[0] - 3, yc - 12), (e_pin[0] + 4, yc - 12))
    s.wire((e_pin[0] + 4, yc - 12), (e_pin[0], yc - 12))
    emid = ((rs_.pin(2)[1] + e_pin[1]) / 2)
    s.wire((e_pin[0], yc - 12), (e_pin[0], e_pin[1]))
    s.junction((e_pin[0], snap(yc - 12)))
    opo = op.pin("1" if unit == 1 else "7")
    rb = R(s, f"R{29 if name=='TEMP' else (18 if name=='OIL' else 19)}", "1k",
           (opo[0] + b_pin[0]) / 2, opo[1], rot=90)
    s.wire(opo, rb.pin(1))
    s.wire(rb.pin(2), (b_pin[0], rb.pin(2)[1]), b_pin)
    # collector -> sender terminal + RC filter -> series R -> clamp -> ADC
    node = (c_pin[0], yc + 14)
    s.wire(c_pin, node)
    tpin = j3.pin(CHANNELS.index((name, rset_v, uref, unit, rset_ref, rf_ref, cf_ref, rs_ref, d_ref, adc, yc)) + 1)
    s.wire(tpin, (tpin[0] + 4, tpin[1]))
    s.label((tpin[0] + 4, tpin[1]), f"SENDER_{name}")
    s.wire(node, (node[0] - 8, node[1]))
    s.label((node[0] - 8, node[1]), f"SENDER_{name}")
    s.junction(node)
    rf = R(s, rf_ref, "1k", node[0] + 10, node[1], rot=90)
    s.wire(node, rf.pin(1))
    cf = C(s, cf_ref, "100nF", rf.pin(2)[0] + 5, node[1] + 9)
    s.wire(rf.pin(2), (cf.pin(1)[0], node[1]))
    s.junction((cf.pin(1)[0], node[1]))
    s.wire((cf.pin(1)[0], node[1]), cf.pin(1))
    gnd(s, cf.pin(2))
    rs2 = R(s, rs_ref, "4.7k", cf.pin(1)[0] + 12, node[1], rot=90)
    s.wire((cf.pin(1)[0], node[1]), rs2.pin(1))
    cn = (rs2.pin(2)[0] + 6, node[1])
    s.wire(rs2.pin(2), cn)
    clamp99(s, d_ref, (cn[0], cn[1] - 0))
    # clamp99 places pin3 at cn
    s.junction(cn)
    s.wire(cn, (cn[0] + 8, cn[1]))
    s.glabel((cn[0] + 8, cn[1]), adc, shape="output")

# unused LM358 unit U4B: follower to GND
u4b = s.place("Amplifier_Operational:LM358", "U4", "LM358", (50, 250), unit=2,
              ref_at=(47, 256), val_at=(53, 256))
s.wire(u4b.pin("6"), (u4b.pin("6")[0] - 3, u4b.pin("6")[1]),
       (u4b.pin("6")[0] - 3, u4b.pin("6")[1] - 8),
       (u4b.pin("7")[0] + 3, u4b.pin("7")[1] - 8), (u4b.pin("7")[0] + 3, u4b.pin("7")[1]), u4b.pin("7"))
gnd(s, u4b.pin("5"))
# opamp power units (unit 3)
for uref, xp in (("U3", 90.0), ("U4", 110.0)):
    pu = s.place("Amplifier_Operational:LM358", uref, "LM358", (xp, 252), unit=3,
                 ref_at=(xp - 3, 244), val_at=(xp + 1.5, 244))
    rail(s, "+12V", pu.pin("8"))
    gnd(s, pu.pin("4"))
    cb = C(s, f"C6{3 if uref=='U3' else 4}", "100nF", xp + 10, 250)
    rail(s, "+12V", cb.pin(1))
    gnd(s, cb.pin(2))
s.text((45, 268), "LM358 V+ = +12V: input CM range must stay >=1.5V below V+;"
                  " 4.5V ref OK. Current source from +5V rail.", size=1.5)

# --- battery sense
bx, by = 200.0, 60.0
s.text((bx - 10, by - 12), "Battery voltage sense (/15.7)", size=1.7)
r85 = R(s, "R85", "100k 1%", bx, by, rot=90)
rail(s, "+12V", (r85.pin(1)[0] - 5, by))
s.wire((r85.pin(1)[0] - 5, by), r85.pin(1))
n1 = (r85.pin(2)[0] + 4, by)
s.wire(r85.pin(2), n1)
r86 = R(s, "R86", "22k 1%", n1[0], by + 10)
s.junction(n1)
s.wire(n1, r86.pin(1))
gnd(s, r86.pin(2))
r87 = R(s, "R87", "4.7k", n1[0] + 10, by, rot=90)
s.wire(n1, r87.pin(1))
n2 = (r87.pin(2)[0] + 6, by)
s.wire(r87.pin(2), n2)
c85 = C(s, "C85", "100nF", n2[0], by + 10)
s.junction(n2)
s.wire(n2, c85.pin(1))
gnd(s, c85.pin(2))
clamp99(s, "D81", (n2[0], n2[1] - 0))
s.wire(n2, (n2[0] + 8, n2[1]))
s.glabel((n2[0] + 8, n2[1]), "ADC_VBAT", shape="output")

# --- D+ excite + sense
dx, dy = 200.0, 120.0
s.text((dx - 10, dy - 12), "Charge alternator D+ excite (~100mA) + sense", size=1.7)
r88 = R(s, "R88", "120R 3W", dx, dy, rot=90)
s.glabel((r88.pin(1)[0] - 5, dy), "+12V_SW", rot=180)
s.wire((r88.pin(1)[0] - 5, dy), r88.pin(1))
dnode = (r88.pin(2)[0] + 4, dy)
s.wire(r88.pin(2), dnode)
s.junction(dnode)
j13 = conn(s, "J13", "Screw_Terminal_DPLUS", 2, dx + 8, dy + 22)
s.wire(dnode, (dnode[0], dy + 14))
s.wire_v_then_h((dnode[0], dy + 14), j13.pin(1))
gnd(s, j13.pin(2))
r85a = R(s, "R185", "100k 1%", dnode[0] + 10, dy, rot=90)
s.wire(dnode, r85a.pin(1))
dn1 = (r85a.pin(2)[0] + 4, dy)
s.wire(r85a.pin(2), dn1)
r86a = R(s, "R186", "22k 1%", dn1[0], dy + 10)
s.junction(dn1)
s.wire(dn1, r86a.pin(1))
gnd(s, r86a.pin(2))
r87a = R(s, "R187", "4.7k", dn1[0] + 10, dy, rot=90)
s.wire(dn1, r87a.pin(1))
dn2 = (r87a.pin(2)[0] + 6, dy)
s.wire(r87a.pin(2), dn2)
c86 = C(s, "C86", "100nF", dn2[0], dy + 10)
s.junction(dn2)
s.wire(dn2, c86.pin(1))
gnd(s, c86.pin(2))
clamp99(s, "D82", dn2)
s.wire(dn2, (dn2[0] + 8, dn2[1]))
s.glabel((dn2[0] + 8, dn2[1]), "ADC_DPLUS", shape="output")

# =================================================================
# Sheet 3 — AC SENSING (6x voltage, 3x CT, VREF_MID buffer)
# =================================================================
s = p.new_sheet("AC sensing", "ac_sense.kicad_sch")
s.text((20, 18), "6x AC voltage dividers (/237, biased at VREF_MID), 3x CT burden+amp,"
                 " 1.65V buffered midpoint. N terminals tie to board GND.", size=2.0)

# VREF_MID buffer (top right)
vx, vy = 260.0, 40.0
r90 = R(s, "R90", "10k 1%", vx, vy)
r91 = R(s, "R91", "10k 1%", vx, vy + 12)
rail(s, "+3V3", r90.pin(1))
s.wire(r90.pin(2), r91.pin(1))
gnd(s, r91.pin(2))
vm = ((r90.pin(2)[1] + r91.pin(1)[1]) / 2)
c90 = C(s, "C90", "100nF", vx - 10, vm + 8)
s.wire((vx, vm), (c90.pin(1)[0], vm))
s.junction((vx, vm))
s.wire((c90.pin(1)[0], vm), c90.pin(1))
gnd(s, c90.pin(2))
u8 = s.place("Amplifier_Operational:MCP6002-xSN", "U8", "MCP6002", (vx + 20, vm), unit=2,
             ref_at=(vx + 17, vm + 6), val_at=(vx + 23, vm + 6))
s.wire((vx, vm), u8.pin("5"))
r92 = R(s, "R92", "47R", u8.pin("7")[0] + 6, vm, rot=90)
s.wire(u8.pin("7"), r92.pin(1))
vrn = (r92.pin(2)[0] + 5, vm)
s.wire(r92.pin(2), vrn)
s.junction(vrn)
c91 = C(s, "C91", "10uF", vrn[0], vm + 9)
s.wire(vrn, c91.pin(1))
gnd(s, c91.pin(2))
# dual feedback (reviewer finding M5): R93 takes DC feedback from after the
# 47R isolation resistor; C93 closes the loop at AC directly from the output
# so the R92/C91 pole sits outside the fast loop.
inv8 = u8.pin("6")
fcol = inv8[0] - 3
s.wire(inv8, (fcol, inv8[1]), (fcol, vm - 12))
r93 = R(s, "R93", "10k", (fcol + vrn[0]) / 2, vm - 10, rot=90)
s.wire((fcol, vm - 10), r93.pin(1))
s.junction((fcol, snap(vm - 10)))
s.wire(r93.pin(2), (vrn[0], vm - 10), (vrn[0], vrn[1]))
coutx = u8.pin("7")[0] + 1.27
c93 = C(s, "C93", "100nF", (fcol + coutx) / 2, vm - 12, rot=90)
s.wire((fcol, vm - 12), c93.pin(1))
s.wire(c93.pin(2), (coutx, vm - 12), (coutx, vm))
s.junction((snap(coutx), snap(vm)))
s.wire(vrn, (vrn[0] + 8, vm))
s.glabel((vrn[0] + 8, vm), "VREF_MID", shape="output")
u8p = s.place("Amplifier_Operational:MCP6002-xSN", "U8", "MCP6002", (vx + 20, vy + 42), unit=3,
              ref_at=(vx + 17, vy + 34), val_at=(vx + 22, vy + 34))
rail(s, "+3V3", u8p.pin("8"))
gnd(s, u8p.pin("4"))
c92 = C(s, "C92", "100nF", vx + 32, vy + 40)
rail(s, "+3V3", c92.pin(1))
gnd(s, c92.pin(2))
# U8 unit A tied off (reviewer finding M3: it was never placed -> floating
# CMOS inputs on the same die as the VREF buffer)
u8a = s.place("Amplifier_Operational:MCP6002-xSN", "U8", "MCP6002", (vx + 62, vy + 42), unit=1,
              ref_at=(vx + 59, vy + 48), val_at=(vx + 65, vy + 48))
s.wire(u8a.pin("2"), (u8a.pin("2")[0] - 3, u8a.pin("2")[1]),
       (u8a.pin("2")[0] - 3, u8a.pin("2")[1] - 8),
       (u8a.pin("1")[0] + 3, u8a.pin("1")[1] - 8),
       (u8a.pin("1")[0] + 3, u8a.pin("1")[1]), u8a.pin("1"))
gnd(s, u8a.pin("3"))

# AC voltage channels
j5 = conn(s, "J5", "Screw_Terminal_GEN", 4, 25, 60)
j6 = conn(s, "J6", "Screw_Terminal_MAINS", 4, 25, 170)
s.text((18, 48), "J5: GEN L1 L2 L3 N   (415V 3ph star, 240V L-N)", size=1.5)
s.text((18, 158), "J6: MAINS L1 L2 L3 N", size=1.5)
gnd(s, j5.pin(4))
gnd(s, j6.pin(4))

AC_CH = [
    # per channel: series R{b}..R{b+3}, bottom R{b+4}, filter R{b+5}, cap, clamp
    ("GEN_L1",   j5, 1, 300, "D40", "C40", "ACV_GEN_L1",   55.0),
    ("GEN_L2",   j5, 2, 310, "D41", "C41", "ACV_GEN_L2",   85.0),
    ("GEN_L3",   j5, 3, 320, "D42", "C42", "ACV_GEN_L3",   115.0),
    ("MAINS_L1", j6, 1, 330, "D43", "C43", "ACV_MAINS_L1", 165.0),
    ("MAINS_L2", j6, 2, 340, "D44", "C44", "ACV_MAINS_L2", 195.0),
    ("MAINS_L3", j6, 3, 350, "D45", "C45", "ACV_MAINS_L3", 225.0),
]
for name, jc, jpin, base, dref, cf, adc, yc in AC_CH:
    tp = jc.pin(jpin)
    s.wire(tp, (tp[0] + 4, tp[1]))
    s.label((tp[0] + 4, tp[1]), f"AC_{name}")
    x0 = 60.0
    s.wire((x0 - 6, yc), (x0 - 10, yc))
    s.label((x0 - 10, yc), f"AC_{name}")
    prev = (x0 - 6, yc)
    for i in range(4):
        rr = R(s, f"R{base+i}", "330k 1206", x0 + i * 12, yc, rot=90)
        s.wire(prev, rr.pin(1))
        prev = rr.pin(2)
    n1 = (prev[0] + 4, yc)
    s.wire(prev, n1)
    s.junction(n1)
    rb = R(s, f"R{base+4}", "5.62k 1%", n1[0], yc + 10)
    s.wire(n1, rb.pin(1))
    s.wire(rb.pin(2), (rb.pin(2)[0], rb.pin(2)[1] + 3))
    s.glabel((rb.pin(2)[0], rb.pin(2)[1] + 3), "VREF_MID", rot=270)
    clamp99(s, dref, n1)
    rff = R(s, f"R{base+5}", "1k", n1[0] + 10, yc, rot=90)
    s.wire(n1, rff.pin(1))
    n2 = (rff.pin(2)[0] + 5, yc)
    s.wire(rff.pin(2), n2)
    s.junction(n2)
    cff = C(s, cf, "22nF C0G", n2[0], yc + 9)
    s.wire(n2, cff.pin(1))
    gnd(s, cff.pin(2))
    s.wire(n2, (n2[0] + 8, n2[1]))
    s.glabel((n2[0] + 8, n2[1]), adc, shape="output")

# CT channels
j7 = conn(s, "J7", "Screw_Terminal_CT", 6, 25, 262)
s.text((18, 250), "J7: CT1 S1/S2, CT2 S1/S2, CT3 S1/S2 (200:5 CTs on load cables)", size=1.5)
CT_CH = [
    ("CT1", 1, 2, "R50", "D50", "U9", 1, "R51", "R52", "R53", "C50", "ACI_CT1", 130.0),
    ("CT2", 3, 4, "R55", "D51", "U9", 2, "R56", "R57", "R58", "C51", "ACI_CT2", 175.0),
    ("CT3", 5, 6, "R59", "D52", "U10", 1, "R64", "R65", "R66", "C52", "ACI_CT3", 220.0),
]
for name, p1, p2, rb_ref, dref, uref, unit, r51, r52, r53, c50, adc, yc in CT_CH:
    t1, t2 = j7.pin(p1), j7.pin(p2)
    s.wire(t1, (t1[0] + 4, t1[1]))
    s.label((t1[0] + 4, t1[1]), f"{name}_P")
    s.wire(t2, (t2[0] + 4, t2[1]))
    s.label((t2[0] + 4, t2[1]), f"{name}_M")
    x0 = 240.0
    top = (x0, yc)
    bot = (x0, yc + 12)
    s.label(top, f"{name}_P", rot=180)
    s.label(bot, f"{name}_M", rot=180)
    # TVS across terminals, burden across, low side to VREF_MID
    # (rot 90: pin2 = top pin, pin1 = bottom pin — wire each to its own row)
    dt = s.place("Device:D_TVS", dref, "SMBJ6.0CA", (x0 + 6, yc + 6), rot=90,
                 ref_at=(x0 + 8.5, yc + 4), val_at=(x0 + 8.5, yc + 7))
    s.wire(top, (dt.pin(2)[0], yc), dt.pin(2))
    s.junction((dt.pin(2)[0], yc))
    s.wire(bot, (dt.pin(1)[0], yc + 12), dt.pin(1))
    s.junction((dt.pin(1)[0], yc + 12))
    rbdn = R(s, rb_ref, "0.05R 3W", x0 + 14, yc + 6)
    s.wire((dt.pin(2)[0], yc), (rbdn.pin(1)[0], yc), rbdn.pin(1))
    s.wire((dt.pin(1)[0], yc + 12), (rbdn.pin(2)[0], yc + 12), rbdn.pin(2))
    s.junction((rbdn.pin(1)[0], yc))
    s.junction((rbdn.pin(2)[0], yc + 12))
    s.wire((rbdn.pin(2)[0], yc + 12), (rbdn.pin(2)[0] + 8, yc + 12))
    s.glabel((rbdn.pin(2)[0] + 8, yc + 12), "VREF_MID", shape="input")
    # amp: non-inverting gain 2 biased at VREF_MID
    op = s.place("Amplifier_Operational:MCP6002-xSN", uref, "MCP6002", (x0 + 45, yc + 2.54),
                 unit=unit, ref_at=(x0 + 42, yc + 8.5), val_at=(x0 + 48, yc + 8.5))
    plus = op.pin("3" if unit == 1 else "5")
    minus = op.pin("2" if unit == 1 else "6")
    outp = op.pin("1" if unit == 1 else "7")
    s.wire((rbdn.pin(1)[0], yc), (plus[0] - 4, yc))
    s.wire((plus[0] - 4, yc), (plus[0] - 4, plus[1]), plus)
    rg1 = R(s, r51, "10k 1%", minus[0] - 6, minus[1] + 8)
    s.wire(minus, (minus[0] - 3, minus[1]))
    s.wire((minus[0] - 3, minus[1]), (rg1.pin(1)[0], minus[1] + 2) if False else (minus[0] - 3, minus[1]))
    s.wire_v_then_h((minus[0] - 3, minus[1]), rg1.pin(1))
    s.glabel((rg1.pin(2)[0], rg1.pin(2)[1] + 3), "VREF_MID", rot=270)
    s.wire(rg1.pin(2), (rg1.pin(2)[0], rg1.pin(2)[1] + 3))
    rg2 = R(s, r52, "10k 1%", minus[0] + 5, minus[1] - 9, rot=90)
    s.wire((minus[0] - 3, minus[1]), (minus[0] - 3, rg2.pin(1)[1]), rg2.pin(1))
    s.junction((minus[0] - 3, minus[1]))
    s.wire(rg2.pin(2), (outp[0] + 3, rg2.pin(2)[1]), (outp[0] + 3, outp[1]))
    s.junction((outp[0] + 3, outp[1]))
    s.wire(outp, (outp[0] + 3, outp[1]))
    r53_ = R(s, r53, "6.8k 1% (phase match)", outp[0] + 10, outp[1], rot=90)
    s.wire((outp[0] + 3, outp[1]), r53_.pin(1))
    n2 = (r53_.pin(2)[0] + 5, outp[1])
    s.wire(r53_.pin(2), n2)
    s.junction(n2)
    cn = C(s, c50, "22nF C0G", n2[0], outp[1] + 9)
    s.wire(n2, cn.pin(1))
    gnd(s, cn.pin(2))
    s.wire(n2, (n2[0] + 8, n2[1]))
    s.glabel((n2[0] + 8, n2[1]), adc, shape="output")
# MCP6002 power units
for uref, xp in (("U9", 170.0), ("U10", 205.0)):
    pu = s.place("Amplifier_Operational:MCP6002-xSN", uref, "MCP6002", (xp, 268), unit=3,
                 ref_at=(xp - 3, 260), val_at=(xp + 1.5, 260))
    rail(s, "+3V3", pu.pin("8"))
    gnd(s, pu.pin("4"))
    cb = C(s, f"C9{6 if uref=='U9' else 7}", "100nF", xp + 10, 266)
    rail(s, "+3V3", cb.pin(1))
    gnd(s, cb.pin(2))
# U10 unit 2 unused -> follower to GND
u10b = s.place("Amplifier_Operational:MCP6002-xSN", "U10", "MCP6002", (240, 268), unit=2,
               ref_at=(237, 274), val_at=(243, 274))
s.wire(u10b.pin("6"), (u10b.pin("6")[0] - 3, u10b.pin("6")[1]),
       (u10b.pin("6")[0] - 3, u10b.pin("6")[1] - 8),
       (u10b.pin("7")[0] + 3, u10b.pin("7")[1] - 8), (u10b.pin("7")[0] + 3, u10b.pin("7")[1]), u10b.pin("7"))
gnd(s, u10b.pin("5"))

# =================================================================
# Sheet 4 — DIGITAL INPUTS + MPU
# =================================================================
s = p.new_sheet("Digital inputs + MPU", "digital.kicad_sch")
s.text((20, 18), "8x 24V digital inputs (divider + clamp, fw debounce);"
                 " MPU pickup -> LM2903 comparator with +/-80mV hysteresis.", size=2.0)

j2 = conn(s, "J2", "Screw_Terminal_DIN", 10, 25, 60)
s.text((18, 46), "J2: DIN1..DIN8 + 2x GND. Switch to +12V (default) or GND"
                 " (fit JP per channel).", size=1.5)
gnd(s, j2.pin(9))
gnd(s, j2.pin(10))
for i in range(8):
    yc = 55.0 + i * 28
    tp = j2.pin(i + 1)
    s.wire(tp, (tp[0] + 4, tp[1]))
    s.label((tp[0] + 4, tp[1]), f"DIN{i+1}_T")
    x0 = 62.0
    s.label((x0 - 6, yc), f"DIN{i+1}_T", rot=180)
    # pull-up R23x + solder jumper for GND-side switches (terminal side of series R)
    r13 = R(s, f"R{231+i}", "5.6k 0.5W", x0 + 2, yc - 18)
    jp = s.place("Jumper:SolderJumper_2_Open", f"JP{4+i}", "GND-side sw", (x0 + 2, yc - 6.5), rot=270,
                 ref_at=(x0 + 5, yc - 8), val_at=(x0 + 5, yc - 5.5))
    rail(s, "+12V", r13.pin(1))
    s.wire(r13.pin(2), jp.pin(1) if jp.pin(1)[1] < jp.pin(2)[1] else jp.pin(2))
    jlow = jp.pin(2) if jp.pin(1)[1] < jp.pin(2)[1] else jp.pin(1)
    s.wire(jlow, (x0 + 2, yc))
    r10 = R(s, f"R{201+i}", "5.6k 0.5W", x0 + 10, yc, rot=90)
    s.wire((x0 - 6, yc), (x0 + 2, yc))
    s.junction((x0 + 2, yc))
    s.wire((x0 + 2, yc), r10.pin(1))
    n1 = (r10.pin(2)[0] + 4, yc)
    s.wire(r10.pin(2), n1)
    s.junction(n1)
    r11 = R(s, f"R{211+i}", "1.8k", n1[0], yc + 9)
    s.wire(n1, r11.pin(1))
    gnd(s, r11.pin(2))
    c10 = C(s, f"C{201+i}", "1uF 50V", n1[0] + 8, yc + 9)
    s.wire(n1, (c10.pin(1)[0], yc))
    s.junction((c10.pin(1)[0], yc))
    s.wire((c10.pin(1)[0], yc), c10.pin(1))
    gnd(s, c10.pin(2))
    r12 = R(s, f"R{221+i}", "10k", c10.pin(1)[0] + 10, yc, rot=90)
    s.wire((c10.pin(1)[0], yc), r12.pin(1))
    n3 = (r12.pin(2)[0] + 6, yc)
    s.wire(r12.pin(2), n3)
    d10 = s.place("Diode:BAT54S", f"D1{i}", "BAT54S", (n3[0] - 5.08, yc), rot=90,
                  ref_at=(n3[0] - 9.5, yc - 1.3), val_at=(n3[0] - 9.5, yc + 1.3))
    # BAT54S: A(1)->COM(3)->K(2); rot90: 1 bottom, 2 top, 3 right = node
    rail(s, "+3V3", d10.pin(2))
    gnd(s, d10.pin(1))
    s.junction(n3)
    s.wire(n3, (n3[0] + 8, n3[1]))
    s.glabel((n3[0] + 8, n3[1]), f"DIN{i+1}", shape="output")

# --- MPU channel
mx, my = 180.0, 60.0
s.text((mx - 5, my - 14), "MPU / magnetic pickup RPM input", size=1.7)
j4 = conn(s, "J4", "Screw_Terminal_MPU", 2, mx, my + 30)
s.wire(j4.pin(1), (j4.pin(1)[0] + 4, j4.pin(1)[1]))
s.label((j4.pin(1)[0] + 4, j4.pin(1)[1]), "MPU_P")
gnd(s, j4.pin(2))
x0 = mx + 20
s.label((x0 - 6, my), "MPU_P", rot=180)
c30 = C(s, "C30", "100nF 100V", x0 + 4, my, rot=90)
s.wire((x0 - 6, my), c30.pin(1))
r30 = R(s, "R30", "10k", c30.pin(2)[0] + 8, my, rot=90)
s.wire(c30.pin(2), r30.pin(1))
n1 = (r30.pin(2)[0] + 4, my)
s.wire(r30.pin(2), n1)
s.junction(n1)
r31 = R(s, "R31", "100k", n1[0], my - 10)
rail(s, "+3V3", r31.pin(1))
s.wire(r31.pin(2), n1)
r32 = R(s, "R32", "100k", n1[0], my + 10)
s.wire(n1, r32.pin(1))
gnd(s, r32.pin(2))
n2 = (n1[0] + 10, my)
s.wire(n1, n2)
s.junction(n2)
d30 = clamp99(s, "D30", n2, val="BAV99")
u5 = s.place("Comparator:LM2903", "U5", "LM2903", (n2[0] + 22, my - 2.54), unit=1,
             ref_at=(n2[0] + 19, my + 4), val_at=(n2[0] + 25, my + 4))
plus, minus, outp = u5.pin("3"), u5.pin("2"), u5.pin("1")
# + and - pins share an x column; the signal feed and the threshold feed
# must use DIFFERENT routes or they merge (reviewer finding C2).
# +input: signal row -> its own column -> up to the + pin.
s.wire(n2, (plus[0] - 6, my))
s.wire((plus[0] - 6, my), (plus[0] - 6, plus[1]), plus)
# -input: straight DOWN from the pin to the threshold-divider row.
r33 = R(s, "R33", "10k", minus[0] - 8, minus[1] + 10)
r34 = R(s, "R34", "10k", minus[0] - 8, minus[1] + 22)
rail(s, "+3V3", (r33.pin(1)[0], r33.pin(1)[1] - 2))
s.wire((r33.pin(1)[0], r33.pin(1)[1] - 2), r33.pin(1))
s.wire(r33.pin(2), r34.pin(1))
tm = ((r33.pin(2)[1] + r34.pin(1)[1]) / 2)
gnd(s, r34.pin(2))
s.wire(minus, (minus[0], tm), (r33.pin(2)[0], tm))
s.junction((r33.pin(2)[0], tm))
# output pull-up + hysteresis
o = (outp[0] + 6, outp[1])
s.wire(outp, o)
s.junction(o)
r35 = R(s, "R35", "10k", o[0], outp[1] - 10)
rail(s, "+3V3", r35.pin(1))
s.wire(r35.pin(2), o)
# hysteresis routed one column right of the output node so the vertical
# run does not pass through R35's pins
r36 = R(s, "R36", "1M (hyst +/-80mV)", o[0] - 10, my - 22, rot=90)
htap = (o[0] + 4, o[1])
s.junction(htap)
s.wire(htap, (htap[0], my - 22), r36.pin(2))
s.wire(r36.pin(1), (n2[0], my - 22), (n2[0], n2[1]))
s.junction(n2)
s.wire(o, (o[0] + 8, o[1]))
s.glabel((o[0] + 8, o[1]), "MPU_RPM", shape="output")
# LM2903 power unit at +5V (VICM limit), unused unit 2 tied off
u5p = s.place("Comparator:LM2903", "U5", "LM2903", (mx + 20, my + 60), unit=3,
              ref_at=(mx + 17, my + 52), val_at=(mx + 22, my + 52))
rail(s, "+5V", u5p.pin("8"))
gnd(s, u5p.pin("4"))
c31 = C(s, "C31", "100nF", mx + 32, my + 58)
rail(s, "+5V", c31.pin(1))
gnd(s, c31.pin(2))
u5b = s.place("Comparator:LM2903", "U5", "LM2903", (mx + 55, my + 60), unit=2,
              ref_at=(mx + 52, my + 66), val_at=(mx + 58, my + 66))
gnd(s, u5b.pin("5"))
r37 = R(s, "R37", "10k", u5b.pin("6")[0] - 6, u5b.pin("6")[1] - 8)
rail(s, "+3V3", r37.pin(1))
s.wire(r37.pin(2), (r37.pin(2)[0], u5b.pin("6")[1]), u5b.pin("6"))
r38 = R(s, "R38", "10k", u5b.pin("7")[0] + 6, u5b.pin("7")[1] - 10)
rail(s, "+3V3", r38.pin(1))
s.wire(r38.pin(2), (r38.pin(2)[0], u5b.pin("7")[1]), u5b.pin("7"))
s.text((mx + 40, my + 72), "U5B unused: -in=3V3/10k, +in=GND, out pulled up"
                           " (open collector)", size=1.4)

# =================================================================
# Sheet 5 — RELAY DRIVERS
# =================================================================
s = p.new_sheet("Relay drivers", "relays.kicad_sch")
s.text((20, 18), "6x G5LE-1 24V relays, 2N7002K low-side drivers, SS34 flyback."
                 " GEN/MAINS contactor contacts are VOLT-FREE; panel wiring must"
                 " also hardware-interlock the contactors.", size=2.0)

j8 = conn(s, "J8", "Screw_Terminal_RELAY_OUT", 5, 25, 90)
s.text((12, 26), "J8 pin 1 = OUT_COM, the common contact supply. The INSTALLER"
                 " feeds it from a fused source; it is NOT this board's rail, so"
                 " no field current crosses the PCB.", size=1.5)
s.text((12, 36), "J8: 1=OUT_COM 2=FUEL(ECU enable) 3=START 4=AUX1 5=AUX2", size=1.5)
s.text((12, 40), "START is PILOT ONLY - drive an external starter relay coil."
                 " A 12V starter solenoid pulls 20-40A; these contacts are 8A DC.", size=1.5)
j15 = conn(s, "J15", "Screw_Terminal_CONTACTOR", 4, 25, 150)
s.text((14, 140), "J15: GEN COM/NO, MAINS COM/NO (volt-free contactor pairs).", size=1.5)
s.text((14, 144), "Separate block from J8: the two contactor coil circuits ride", size=1.5)
s.text((14, 148), "different AC sources; 8-pole body, odd poles wired (creepage).", size=1.5)

RELAYS = [
    # (name, K, Q, Rgate, Rpd, Dfly, signal, x-column, y, volt-free)
    ("FUEL",    "K1", "Q60", "R60", "R61", "D60", "RLY_FUEL",    90.0,  70.0,  False),
    ("START",   "K2", "Q61", "R62", "R63", "D61", "RLY_START",   90.0,  145.0, False),
    ("GEN",     "K3", "Q62", "R110", "R111", "D62", "RLY_GEN",   90.0,  220.0, True),
    ("MAINS",   "K4", "Q63", "R67", "R68", "D63", "RLY_MAINS",   230.0, 70.0,  True),
    # K5/K6 are configurable (default horn / preheat) — the engine ECU owns
    # the cold-start aid on a CRDi engine, so a fixed PREHEAT channel would
    # often go unused
    ("AUX1",    "K5", "Q64", "R69", "R75", "D64", "RLY_AUX1",    230.0, 145.0, False),
    ("AUX2",    "K6", "Q65", "R76", "R77", "D65", "RLY_AUX2",    230.0, 220.0, False),
]
for name, kref, qref, rg, rpd, dfly, sig, x0, yc, voltfree in RELAYS:
    q = s.place("Transistor_FET:2N7002K", qref, "2N7002K 60V", (x0, yc), rot=0,
                ref_at=(x0 + 5, yc - 2), val_at=(x0 + 5, yc + 0.6))
    gpin, spin_, dpin = q.pin(1), q.pin(2), q.pin(3)
    rg_ = R(s, rg, "100R", gpin[0] - 10, gpin[1], rot=90)
    s.wire(rg_.pin(2), gpin)
    s.wire((rg_.pin(1)[0] - 5, gpin[1]), rg_.pin(1))
    s.glabel((rg_.pin(1)[0] - 5, gpin[1]), sig, rot=180, shape="input")
    rpd_ = R(s, rpd, "100k", gpin[0] - 6, gpin[1] + 10)
    s.wire(gpin, rpd_.pin(1)) if False else None
    s.wire_h_then_v(gpin, rpd_.pin(1))
    s.junction(gpin)
    gnd(s, rpd_.pin(2))
    gnd(s, spin_)
    # G5LE-1 lib symbol pin map (decoded from the symbol graphics, matches
    # the Relay_SPDT_Omron-G5LE-1 footprint): coil = pins 2 (bottom-left)
    # and 5 (top-left); COM = 1 (bottom-right), NO = 3 (top-right),
    # NC = 4 (top-mid). SME review catch: the earlier 1/2-coil 3/4/5-contact
    # assumption netted every relay wrong.
    # Place so the coil-switched pin 2 sits directly above the FET drain.
    k = s.place("Relay:G5LE-1", kref, "G5LE-1 12V", (dpin[0] + 5.08, yc - 18),
                ref_at=(dpin[0] - 6, yc - 34), val_at=(dpin[0] - 6, yc - 31.5))
    coil_sw = k.pin(2)    # bottom-left coil pin, directly above drain
    coil_hot = k.pin(5)   # top-left coil pin -> +12V_SW
    s.wire(coil_sw, dpin)
    rowh = snap(coil_hot[1] - 4)
    s.glabel((coil_hot[0] - 14, rowh), "+12V_SW", rot=180)
    s.wire((coil_hot[0] - 14, rowh), (coil_hot[0], rowh), coil_hot)
    # flyback vertical beside the coil column: K up to the +12V_SW row,
    # A down to the coil-switch/drain run
    xf = snap(coil_hot[0] - 7.62)
    dfly_ = s.place("Device:D_Schottky", dfly, "SS34", (xf, yc - 18), rot=270,
                    ref_at=(xf - 6.5, yc - 19.3), val_at=(xf - 6.5, yc - 16.7))
    kk, aa = dfly_.pin(1), dfly_.pin(2)
    if kk[1] > aa[1]:
        raise SystemExit(f"{dfly}: flyback rot=270 no longer K-top/A-bottom")
    s.wire(kk, (xf, rowh))
    s.junction((xf, rowh))
    ydrop = snap(yc - 6.35)
    s.wire(aa, (xf, ydrop), (coil_sw[0], ydrop))
    s.junction((coil_sw[0], ydrop))
    # contacts: COM bottom-right, NO top-right, NC unused
    com, no_, nc_ = k.pin(1), k.pin(3), k.pin(4)
    s.no_connect(nc_)
    lx = snap(no_[0] + 10)
    ycom = snap(com[1] + 3)
    yno = snap(no_[1] - 3)
    s.wire(com, (com[0], ycom), (lx, ycom))
    s.wire(no_, (no_[0], yno), (lx, yno))
    if voltfree:
        s.label((lx, ycom), f"{name}_COM")
        s.label((lx, yno), f"{name}_NO")
    else:
        s.label((lx, ycom), "OUT_COM")
        s.label((lx, yno), f"{name}_OUT")

# terminal wiring: J8 = common supply + 4 dry NO outputs, J15 = volt-free pairs
for i, net in enumerate(["OUT_COM", "FUEL_OUT", "START_OUT", "AUX1_OUT", "AUX2_OUT"]):
    tp = j8.pin(i + 1)
    s.wire(tp, (tp[0] + 4, tp[1]))
    s.label((tp[0] + 4, tp[1]), net)
for i, net in enumerate(["GEN_COM", "GEN_NO", "MAINS_COM", "MAINS_NO"]):
    tp = j15.pin(i + 1)
    s.wire(tp, (tp[0] + 4, tp[1]))
    s.label((tp[0] + 4, tp[1]), net)
# map relay-side labels to terminal-side labels (same names where needed)
s.text((20, 290), "K1 FUEL = run-enable: drives the engine-ECU enable input (CRDi)"
                  " or the fuel solenoid (legacy). K5/K6 AUX are configurable,"
                  " defaulting to horn and preheat.", size=1.5)

# =================================================================
# Sheet 6 — COMMS (CAN, RS485, EEPROM)
# =================================================================
s = p.new_sheet("Comms", "comms.kicad_sch")
s.text((20, 18), "CAN J1939 (TJA1051T/3, split termination via JP1),"
                 " RS485 Modbus (THVD1450, JP2 term), SPI EEPROM.", size=2.0)

# CAN
u6 = s.place("Interface_CAN_LIN:TJA1051T-3", "U6", "TJA1051T/3", (90, 60),
             ref_at=(84, 44), val_at=(92, 44))
s.wire(u6.pin(1), (u6.pin(1)[0] - 6, u6.pin(1)[1]))
s.glabel((u6.pin(1)[0] - 6, u6.pin(1)[1]), "CAN_TX", rot=180, shape="input")
s.wire(u6.pin(4), (u6.pin(4)[0] - 6, u6.pin(4)[1]))
s.glabel((u6.pin(4)[0] - 6, u6.pin(4)[1]), "CAN_RX", rot=180, shape="output")
s.wire(u6.pin(5), (u6.pin(5)[0] - 6, u6.pin(5)[1]))
rail(s, "+3V3", (u6.pin(5)[0] - 6, u6.pin(5)[1]))
gnd(s, (u6.pin(8)[0] - 6, u6.pin(8)[1] + 0))
s.wire(u6.pin(8), (u6.pin(8)[0] - 6, u6.pin(8)[1]))  # S pin low = high-speed mode
rail(s, "+5V", u6.pin(3))
c65 = C(s, "C65", "100nF", u6.pin(3)[0] + 10, u6.pin(3)[1] + 4)
rail(s, "+5V", c65.pin(1))
gnd(s, c65.pin(2))
gnd(s, u6.pin(2))
canh, canl = u6.pin(7), u6.pin(6)
hx = canh[0] + 12
s.wire(canh, (hx, canh[1]))
s.wire(canl, (hx, canl[1]))
s.junction((hx, canh[1]))
s.junction((hx, canl[1]))
# split termination hangs BELOW the CANL row via a solder jumper:
# CANH row -> down (right of the bus labels) -> JP1 -> R78 -> R79 -> up to CANL
s.wire((hx, canh[1]), (hx + 30, canh[1]))
s.label((hx + 30, canh[1]), "CAN_H_T")
s.wire((hx, canl[1]), (hx + 26, canl[1]))
s.label((hx + 26, canl[1]), "CAN_L_T")
tx = hx + 36
s.wire((hx + 30, canh[1]), (tx, canh[1]))   # extend H bus to the drop column
jp1 = s.place("Jumper:SolderJumper_2_Open", "JP1", "CAN term (bus ends)",
              (tx, canh[1] + 8), rot=270,
              ref_at=(tx + 3, canh[1] + 6), val_at=(tx + 3, canh[1] + 9))
jt1 = jp1.pin(1) if jp1.pin(1)[1] < jp1.pin(2)[1] else jp1.pin(2)
jb1 = jp1.pin(2) if jp1.pin(1)[1] < jp1.pin(2)[1] else jp1.pin(1)
s.wire((tx, canh[1]), jt1)
r78 = R(s, "R78", "60.4R", tx, canh[1] + 17)
s.wire(jb1, r78.pin(1))
r79 = R(s, "R79", "60.4R", tx, canh[1] + 29)
s.wire(r78.pin(2), r79.pin(1))
cm = ((r78.pin(2)[1] + r79.pin(1)[1]) / 2)
c70 = C(s, "C70", "4.7nF", tx + 10, cm + 8)
s.wire((tx, cm), (c70.pin(1)[0], cm))
s.junction((tx, snap(cm)))
s.wire((c70.pin(1)[0], cm), c70.pin(1))
gnd(s, c70.pin(2))
# from R79 bottom, left then up to the CANL row (joins mid-run, junction)
lx = hx + 20
s.wire(r79.pin(2), (tx, r79.pin(2)[1] + 3), (lx, r79.pin(2)[1] + 3), (lx, canl[1]))
s.junction((lx, canl[1]))
j9 = conn(s, "J9", "Screw_Terminal_CAN", 3, 210, canh[1] + 2)
s.wire(j9.pin(1), (j9.pin(1)[0] + 4, j9.pin(1)[1]))
s.label((j9.pin(1)[0] + 4, j9.pin(1)[1]), "CAN_H_T")
s.wire(j9.pin(2), (j9.pin(2)[0] + 4, j9.pin(2)[1]))
s.label((j9.pin(2)[0] + 4, j9.pin(2)[1]), "CAN_L_T")
gnd(s, j9.pin(3))

# RS485
u7 = s.place("Interface_UART:MAX3485", "U7", "THVD1450", (90, 160),
             ref_at=(84, 142), val_at=(92, 142))
s.wire(u7.pin(4), (u7.pin(4)[0] - 6, u7.pin(4)[1]))
s.glabel((u7.pin(4)[0] - 6, u7.pin(4)[1]), "RS485_TX", rot=180, shape="input")
s.wire(u7.pin(1), (u7.pin(1)[0] - 6, u7.pin(1)[1]))
s.glabel((u7.pin(1)[0] - 6, u7.pin(1)[1]), "RS485_RX", rot=180, shape="output")
# DE and /RE tied together to one GPIO
s.wire(u7.pin(2), (u7.pin(2)[0] - 4, u7.pin(2)[1]), (u7.pin(2)[0] - 4, u7.pin(3)[1]), u7.pin(3))
s.junction((u7.pin(2)[0] - 4, u7.pin(3)[1]))
s.wire((u7.pin(2)[0] - 4, u7.pin(3)[1]), (u7.pin(3)[0] - 8, u7.pin(3)[1]))
s.glabel((u7.pin(3)[0] - 8, u7.pin(3)[1]), "RS485_DE", rot=180, shape="input")
rail(s, "+3V3", u7.pin(8))
c66 = C(s, "C66", "100nF", u7.pin(8)[0] + 10, u7.pin(8)[1] + 4)
rail(s, "+3V3", c66.pin(1))
gnd(s, c66.pin(2))
gnd(s, u7.pin(5))
a_, b_ = u7.pin(6), u7.pin(7)
ax = a_[0] + 12
s.wire(a_, (ax, a_[1]))
s.wire(b_, (ax, b_[1]))
s.junction((ax, a_[1]))
s.junction((ax, b_[1]))
# bias + termination
# idle-state fail-safe bias: pull A up, pull B down (A-B > 0 = mark/idle).
# Reviewer finding M2: this was reversed. The vertical runs deliberately
# CROSS the other bus row mid-segment (no junction = no connection).
r73 = R(s, "R73", "560R fail-safe", ax + 6, b_[1] - 12)
rail(s, "+3V3", r73.pin(1))
s.wire(r73.pin(2), (r73.pin(2)[0], a_[1]))
s.junction((r73.pin(2)[0], a_[1]))
r74 = R(s, "R74", "560R fail-safe", ax + 12, a_[1] + 12)
s.wire(r74.pin(1), (r74.pin(1)[0], b_[1]))
s.junction((r74.pin(1)[0], b_[1]))
gnd(s, r74.pin(2))
# bus rows out to labels
s.wire((ax, b_[1]), (ax + 26, b_[1]))
s.label((ax + 26, b_[1]), "RS485_B")
s.wire((ax, a_[1]), (ax + 30, a_[1]))
s.label((ax + 30, a_[1]), "RS485_A")
# termination JP2 + R72 hang below on their own drop column (right of labels)
tx2 = ax + 36
s.wire((ax + 26, b_[1]), (tx2, b_[1]))   # extend B row to drop column
jp2 = s.place("Jumper:SolderJumper_2_Open", "JP2", "485 term", (tx2, b_[1] + 8), rot=270,
              ref_at=(tx2 + 3, b_[1] + 6), val_at=(tx2 + 3, b_[1] + 9))
jt = jp2.pin(1) if jp2.pin(1)[1] < jp2.pin(2)[1] else jp2.pin(2)
jb = jp2.pin(2) if jp2.pin(1)[1] < jp2.pin(2)[1] else jp2.pin(1)
s.wire((tx2, b_[1]), jt)
r72 = R(s, "R72", "120R", tx2, b_[1] + 17)
s.wire(jb, r72.pin(1))
# from R72 bottom, left then up to the A row (joins mid-run, junction)
lx2 = ax + 20
s.wire(r72.pin(2), (tx2, r72.pin(2)[1] + 3), (lx2, r72.pin(2)[1] + 3), (lx2, a_[1]))
s.junction((lx2, a_[1]))
j10 = conn(s, "J10", "Screw_Terminal_RS485", 3, 210, b_[1] + 2)
s.wire(j10.pin(1), (j10.pin(1)[0] + 4, j10.pin(1)[1]))
s.label((j10.pin(1)[0] + 4, j10.pin(1)[1]), "RS485_B")
s.wire(j10.pin(2), (j10.pin(2)[0] + 4, j10.pin(2)[1]))
s.label((j10.pin(2)[0] + 4, j10.pin(2)[1]), "RS485_A")
gnd(s, j10.pin(3))

# EEPROM
u11 = s.place("Memory_EEPROM:M95256-WMN6P", "U11", "M95M02-DR", (90, 240),
              ref_at=(84, 226), val_at=(92, 226))
rail(s, "+3V3", u11.pin(8))
gnd(s, u11.pin(4))
c67 = C(s, "C67", "100nF", u11.pin(8)[0] + 25, u11.pin(8)[1] + 4)
rail(s, "+3V3", c67.pin(1))
gnd(s, c67.pin(2))
cs = u11.pin(1)
r95 = R(s, "R95", "10k", cs[0] - 8, cs[1] - 10)
rail(s, "+3V3", r95.pin(1))
s.wire(r95.pin(2), (r95.pin(2)[0], cs[1]), cs)
s.junction((r95.pin(2)[0], cs[1]))
s.wire(cs, (cs[0] - 14, cs[1]))
s.glabel((cs[0] - 14, cs[1]), "EE_CS", rot=180, shape="input")
for pin, net, shape in (("3", "WP_TIE", None), ("7", "HOLD_TIE", None)):
    pass
r96 = R(s, "R96", "10k", u11.pin(3)[0] - 10, u11.pin(3)[1] - 8)
rail(s, "+3V3", r96.pin(1))
s.wire(r96.pin(2), (r96.pin(2)[0], u11.pin(3)[1]), u11.pin(3))
r97 = R(s, "R97", "10k", u11.pin(7)[0] - 16, u11.pin(7)[1] + 10)
rail(s, "+3V3", (r97.pin(2)[0], r97.pin(2)[1] + 2)) if False else None
s.wire(u11.pin(7), (r97.pin(1)[0], u11.pin(7)[1]), r97.pin(1))
rail(s, "+3V3", (r97.pin(2)[0], r97.pin(2)[1]))
s.wire(u11.pin(6), (u11.pin(6)[0] + 6, u11.pin(6)[1]))
s.glabel((u11.pin(6)[0] + 6, u11.pin(6)[1]), "SPI2_SCK", shape="input")
s.wire(u11.pin(5), (u11.pin(5)[0] + 6, u11.pin(5)[1]))
s.glabel((u11.pin(5)[0] + 6, u11.pin(5)[1]), "SPI2_MOSI", shape="input")
s.wire(u11.pin(2), (u11.pin(2)[0] + 6, u11.pin(2)[1]))
s.glabel((u11.pin(2)[0] + 6, u11.pin(2)[1]), "SPI2_MISO", shape="output")

# =================================================================
# Sheet 7 — MCU
# =================================================================
s = p.new_sheet("MCU", "mcu.kicad_sch")
s.text((20, 18), "STM32F407VGT6 LQFP100. HSE 8MHz (PH0/PH1), LSE 32.768kHz"
                 " (PC14/15), SWD via TC2030, supercap RTC backup.", size=2.0)

mcu = s.place("MCU_ST_STM32F4:STM32F407VGTx", "U12", "STM32F407VGT6", (200, 160),
              ref_at=(178, 96), val_at=(192, 96))

PINMAP = {
    "PA0": ("ACI_CT1", "input"), "PA1": ("ACI_CT2", "input"), "PA2": ("ACI_CT3", "input"),
    "PA3": ("ADC_OIL", "input"), "PA4": ("ADC_FUEL", "input"), "PA5": ("ADC_TEMP", "input"),
    "PA6": ("ADC_VBAT", "input"), "PA7": ("ADC_DPLUS", "input"),
    "PC0": ("ACV_GEN_L1", "input"), "PC1": ("ACV_GEN_L2", "input"), "PC2": ("ACV_GEN_L3", "input"),
    "PC3": ("ACV_MAINS_L1", "input"), "PC4": ("ACV_MAINS_L2", "input"), "PC5": ("ACV_MAINS_L3", "input"),
    "PB3": ("SPI1_SCK", "output"), "PB4": ("SPI1_MISO", "input"), "PB5": ("SPI1_MOSI", "output"),
    "PB6": ("MPU_RPM", "input"),
    "PB7": ("TFT_CS", "output"), "PB8": ("LED_CS", "output"), "PB9": ("KEY_CS", "output"),
    "PB12": ("EE_CS", "output"), "PB13": ("SPI2_SCK", "output"),
    "PB14": ("SPI2_MISO", "input"), "PB15": ("SPI2_MOSI", "output"),
    "PD0": ("CAN_RX", "input"), "PD1": ("CAN_TX", "output"),
    "PD2": ("RLY_FUEL", "output"), "PD3": ("RLY_START", "output"),
    "PD4": ("RLY_GEN", "output"), "PD5": ("RLY_MAINS", "output"),
    "PD6": ("RLY_AUX1", "output"), "PD7": ("RLY_AUX2", "output"),
    "PD8": ("RS485_TX", "output"), "PD9": ("RS485_RX", "input"), "PD10": ("RS485_DE", "output"),
    "PE0": ("DIN1", "input"), "PE1": ("DIN2", "input"), "PE2": ("DIN3", "input"),
    "PE3": ("DIN4", "input"), "PE4": ("DIN5", "input"), "PE5": ("DIN6", "input"),
    "PE6": ("DIN7", "input"), "PE7": ("DIN8", "input"),
    "PE8": ("TFT_RST", "output"), "PE9": ("TFT_INT", "input"), "PE11": ("BL_PWM", "output"),
    "PA9": ("DEBUG_TX", "output"), "PA10": ("DEBUG_RX", "input"),
    "PA13": ("SWDIO", "bidirectional"), "PA14": ("SWCLK", "input"),
    "PA8": ("LED_HB", "output"),
}

# name -> (pin_no, angle); resolve from symbol
name_pins = {}
for uid, pn in [(u.unitId, pin) for u in mcu.sym.units for pin in u.pins]:
    name_pins[pn.name.split("/")[0]] = (str(pn.number), pn.position.angle)

used = set()
for pname, (net, shape) in PINMAP.items():
    num, ang = name_pins[pname]
    xy = mcu.pin(num)
    used.add(num)
    if ang == 0:      # left side, wire goes left
        s.wire(xy, (xy[0] - 5, xy[1]))
        s.glabel((xy[0] - 5, xy[1]), net, rot=180, shape=shape)
    elif ang == 180:  # right side
        s.wire(xy, (xy[0] + 5, xy[1]))
        s.glabel((xy[0] + 5, xy[1]), net, rot=0, shape=shape)

# crystals: OSC pins are only 2.54mm apart, so route each pin to labels and
# build the crystal blocks in free space on the left of the sheet
for pn_in, pn_out, ynet_a, ynet_b in (("PH0", "PH1", "HSE_IN", "HSE_OUT"),
                                      ("PC14", "PC15", "LSE_IN", "LSE_OUT")):
    for pname, net in ((pn_in, ynet_a), (pn_out, ynet_b)):
        num, ang = name_pins[pname]
        xy = mcu.pin(num)
        used.add(num)
        if ang == 0:
            s.wire(xy, (xy[0] - 5, xy[1]))
            s.label((xy[0] - 5, xy[1]), net, rot=180)
        else:
            s.wire(xy, (xy[0] + 5, xy[1]))
            s.label((xy[0] + 5, xy[1]), net)

def crystal_block(refx, refv, capref1, capref2, capval, neta, netb, cx, cy):
    ya, yb = cy, cy + 12.7
    s.label((cx - 8, ya), refv + "_A") if False else None
    yx = s.place("Device:Crystal", refx, refv, (cx, (ya + yb) / 2), rot=270,
                 ref_at=(cx + 4, (ya + yb) / 2 - 1.3), val_at=(cx + 4, (ya + yb) / 2 + 1.3))
    top = yx.pin(1) if yx.pin(1)[1] < yx.pin(2)[1] else yx.pin(2)
    bot = yx.pin(2) if yx.pin(1)[1] < yx.pin(2)[1] else yx.pin(1)
    s.wire((cx - 10, ya), (cx, ya), top)
    s.label((cx - 10, ya), neta, rot=180)
    s.wire((cx - 10, yb), (cx, yb), bot)
    s.label((cx - 10, yb), netb, rot=180)
    # load caps hang below their nodes, clear of the other node's row
    ca = C(s, capref1, capval, cx + 38, ya + 8)
    s.wire((cx, ya), (ca.pin(1)[0], ya))
    s.junction((cx, ya))
    s.wire((ca.pin(1)[0], ya), ca.pin(1))
    gnd(s, ca.pin(2))
    cb = C(s, capref2, capval, cx + 22, yb + 8)
    s.wire((cx, yb), (cb.pin(1)[0], yb))
    s.junction((cx, yb))
    s.wire((cb.pin(1)[0], yb), cb.pin(1))
    gnd(s, cb.pin(2))

crystal_block("Y1", "8MHz", "C72", "C73", "12pF", "HSE_IN", "HSE_OUT", 90, 210)
crystal_block("Y2", "32.768kHz", "C74", "C75", "6.8pF", "LSE_IN", "LSE_OUT", 90, 245)

# NRST, BOOT0
nrst = mcu.pin(name_pins["NRST"][0]); used.add(name_pins["NRST"][0])
s.wire(nrst, (nrst[0] - 6, nrst[1]))
s.label((nrst[0] - 6, nrst[1]), "NRST")
boot0 = mcu.pin(name_pins["BOOT0"][0]); used.add(name_pins["BOOT0"][0])
s.wire(boot0, (boot0[0] - 6, boot0[1]))
s.label((boot0[0] - 6, boot0[1]), "BOOT0")

# support circuitry column (left)
sx, sy = 60.0, 60.0
c71 = C(s, "C71", "100nF", sx, sy + 8)
s.wire((sx, sy), c71.pin(1))
gnd(s, c71.pin(2))
s.wire((sx, sy), (sx + 8, sy))
s.label((sx + 8, sy), "NRST")
s.junction((sx, sy))
r80 = R(s, "R80", "10k", sx + 30, sy + 8)
s.wire((sx + 30, sy), r80.pin(1))
gnd(s, r80.pin(2))
s.wire((sx + 30, sy), (sx + 38, sy))
s.label((sx + 38, sy), "BOOT0")
s.junction((sx + 30, sy))
jp3 = s.place("Jumper:SolderJumper_2_Open", "JP3", "BOOT0->3V3", (sx + 26, sy - 8), rot=90,
              ref_at=(sx + 30, sy - 10), val_at=(sx + 30, sy - 7))
jt3 = jp3.pin(1) if jp3.pin(1)[1] < jp3.pin(2)[1] else jp3.pin(2)
jb3 = jp3.pin(2) if jp3.pin(1)[1] < jp3.pin(2)[1] else jp3.pin(1)
rail(s, "+3V3", jt3)
s.wire(jb3, (jb3[0], sy), (sx + 30, sy))

# VBAT supercap
vb = mcu.pin(name_pins["VBAT"][0]); used.add(name_pins["VBAT"][0])
s.wire(vb, (vb[0], vb[1] - 4))
s.label((vb[0], vb[1] - 4), "VBAT_RTC", rot=90)
# rot 180 puts A left / K right: charge current flows +3V3 -> D70 -> R81
# -> supercap (rot 90 made it vertical and reversed — reviewer finding C3)
d70 = s.place("Device:D_Schottky", "D70", "BAT54", (sx + 4, sy + 40), rot=180,
              ref_at=(sx, sy + 36), val_at=(sx + 8, sy + 36))
a70, k70 = d70.pin(2), d70.pin(1)   # rot 180: pin2 A left, pin1 K right
rail(s, "+3V3", (a70[0] - 4, sy + 40))
s.wire((a70[0] - 4, sy + 40), a70)
r81 = R(s, "R81", "330R", k70[0] + 8, sy + 40, rot=90)
s.wire(k70, r81.pin(1))
vbn = (r81.pin(2)[0] + 5, sy + 40)
s.wire(r81.pin(2), vbn)
s.junction(vbn)
c76 = CP(s, "C76", "0.22F supercap", vbn[0], sy + 50)
s.wire(vbn, c76.pin(1))
gnd(s, c76.pin(2))
s.wire(vbn, (vbn[0] + 8, vbn[1]))
s.label((vbn[0] + 8, vbn[1]), "VBAT_RTC")

# VDD / VDDA / VCAP / VREF+
vdd_pins = [str(pn.number) for u in mcu.sym.units for pn in u.pins if pn.name == "VDD"]
vss_pins = [str(pn.number) for u in mcu.sym.units for pn in u.pins if pn.name == "VSS"]
# one horizontal rail above/below the package instead of a symbol per pin
vdd_xy = [mcu.pin(n) for n in vdd_pins]
ytop_rail = min(y for _, y in vdd_xy) - 6
for (x, y), n in zip(vdd_xy, vdd_pins):
    s.wire((x, y), (x, ytop_rail)); used.add(n)
s.wire((min(x for x, _ in vdd_xy), ytop_rail), (max(x for x, _ in vdd_xy), ytop_rail))
for x, _ in vdd_xy[1:-1]:
    s.junction((x, ytop_rail))
rail(s, "+3V3", (min(x for x, _ in vdd_xy), ytop_rail))
vss_xy = [mcu.pin(n) for n in vss_pins]
ybot_rail = max(y for _, y in vss_xy) + 6
for (x, y), n in zip(vss_xy, vss_pins):
    s.wire((x, y), (x, ybot_rail)); used.add(n)
s.wire((min(x for x, _ in vss_xy), ybot_rail), (max(x for x, _ in vss_xy), ybot_rail))
for x, _ in vss_xy[1:-1]:
    s.junction((x, ybot_rail))
gnd(s, (max(x for x, _ in vss_xy), ybot_rail))
vcap1 = [str(pn.number) for u in mcu.sym.units for pn in u.pins if pn.name == "VCAP_1"][0]
vcap2 = [str(pn.number) for u in mcu.sym.units for pn in u.pins if pn.name == "VCAP_2"][0]
for i, n in enumerate((vcap1, vcap2)):
    xy = mcu.pin(n); used.add(n)
    # VCAP pins are on the left side; wire left, staggered so labels clear
    s.wire(xy, (xy[0] - 5 - 6 * i, xy[1]))
    s.label((xy[0] - 5 - 6 * i, xy[1]), f"VCAP{i+1}", rot=180)
c78 = C(s, "C78", "2.2uF", sx + 60, sy + 48)
s.wire((sx + 60, sy + 40), c78.pin(1))
s.label((sx + 60, sy + 40), "VCAP1", rot=0)
s.wire((sx + 60, sy + 40), c78.pin(1))
gnd(s, c78.pin(2))
c79 = C(s, "C79", "2.2uF", sx + 75, sy + 48)
s.wire((sx + 75, sy + 40), c79.pin(1))
s.label((sx + 75, sy + 40), "VCAP2", rot=0)
gnd(s, c79.pin(2))
# VDDA via ferrite
vdda = [str(pn.number) for u in mcu.sym.units for pn in u.pins if pn.name == "VDDA"][0]
vref = [str(pn.number) for u in mcu.sym.units for pn in u.pins if pn.name == "VREF+"][0]
vssa = [str(pn.number) for u in mcu.sym.units for pn in u.pins if pn.name == "VSSA"][0]
xy = mcu.pin(vdda); used.add(vdda)
s.wire(xy, (xy[0], xy[1] - 4))
s.label((xy[0], xy[1] - 4), "VDDA_F", rot=90)
xy = mcu.pin(vref); used.add(vref)
s.wire(xy, (xy[0], xy[1] - 4))
s.label((xy[0], xy[1] - 4), "VDDA_F", rot=90)
xy = mcu.pin(vssa); used.add(vssa)
gnd(s, xy)
fb2 = s.place("Device:FerriteBead", "FB2", "600R@100MHz", (sx + 4, sy + 75), rot=90,
              ref_at=(sx, sy + 70), val_at=(sx, sy + 72.5))
rail(s, "+3V3", (fb2.pin(1)[0] - 4, sy + 75))
s.wire((fb2.pin(1)[0] - 4, sy + 75), fb2.pin(1))
fn = (fb2.pin(2)[0] + 4, sy + 75)
s.wire(fb2.pin(2), fn)
s.junction(fn)
c80 = C(s, "C80", "1uF", fn[0], sy + 84)
s.wire(fn, c80.pin(1))
gnd(s, c80.pin(2))
c81 = C(s, "C81", "10nF", fn[0] + 8, sy + 84)
s.wire(fn, (c81.pin(1)[0], fn[1]))
s.junction((c81.pin(1)[0], fn[1]))
s.wire((c81.pin(1)[0], fn[1]), c81.pin(1))
gnd(s, c81.pin(2))
s.wire((c81.pin(1)[0], fn[1]), (c81.pin(1)[0] + 6, fn[1]))
s.label((c81.pin(1)[0] + 6, fn[1]), "VDDA_F")

# decoupling bank
s.text((sx - 5, sy + 100), "VDD decoupling at each pin pair:", size=1.5)
for i in range(6):
    cd = C(s, f"C10{i}", "100nF", sx + i * 10, sy + 112)
    rail(s, "+3V3", cd.pin(1))
    gnd(s, cd.pin(2))
cbulk = CP(s, "C77", "4.7uF", sx + 60, sy + 112)
rail(s, "+3V3", cbulk.pin(1))
gnd(s, cbulk.pin(2))

# LED heartbeat
r99 = R(s, "R99", "1k", sx + 5, sy + 140)
# rot 90: A top (to R99), K bottom (to GND) — rot 270 was reversed (M4)
led = s.place("Device:LED", "DS1", "green", (sx + 5, sy + 152), rot=90,
              ref_at=(sx + 9, sy + 150), val_at=(sx + 9, sy + 152.5))
s.wire((sx + 5, sy + 132), r99.pin(1))
s.glabel((sx + 5, sy + 132), "LED_HB", rot=90, shape="input")
lk = led.pin(1); la = led.pin(2)
ltop = la if la[1] < lk[1] else lk
lbot = lk if la[1] < lk[1] else la
s.wire(r99.pin(2), (r99.pin(2)[0], ltop[1]), ltop) if abs(r99.pin(2)[0]-ltop[0]) > 0.05 else s.wire(r99.pin(2), ltop)
gnd(s, lbot)

# SWD connector
sw = s.place("Connector:Conn_ARM_SWD_TagConnect_TC2030", "J11", "TC2030 SWD", (320, 80),
             ref_at=(312, 66), val_at=(322, 66))
rail(s, "+3V3", sw.pin(1))
gnd(s, sw.pin(5))
s.wire(sw.pin(2), (sw.pin(2)[0] + 6, sw.pin(2)[1]))
s.glabel((sw.pin(2)[0] + 6, sw.pin(2)[1]), "SWDIO")
s.wire(sw.pin(4), (sw.pin(4)[0] + 6, sw.pin(4)[1]))
s.glabel((sw.pin(4)[0] + 6, sw.pin(4)[1]), "SWCLK")
s.wire(sw.pin(3), (sw.pin(3)[0] + 6, sw.pin(3)[1]))
s.label((sw.pin(3)[0] + 6, sw.pin(3)[1]), "NRST")
s.no_connect(sw.pin(6))

# debug UART header
j14 = s.place("Connector_Generic:Conn_01x03", "J14", "DEBUG_UART", (320, 130),
              ref_at=(316, 122), val_at=(316, 140))
s.wire(j14.pin(1), (j14.pin(1)[0] - 6, j14.pin(1)[1]))
s.glabel((j14.pin(1)[0] - 6, j14.pin(1)[1]), "DEBUG_TX", rot=180)
s.wire(j14.pin(2), (j14.pin(2)[0] - 6, j14.pin(2)[1]))
s.glabel((j14.pin(2)[0] - 6, j14.pin(2)[1]), "DEBUG_RX", rot=180)
gnd(s, j14.pin(3))

# NC on all unused MCU pins
for u in mcu.sym.units:
    for pn in u.pins:
        n = str(pn.number)
        if n not in used:
            s.no_connect(mcu.pin(n))

# =================================================================
# Sheet 8 — DISPLAY CONNECTOR
# =================================================================
s = p.new_sheet("Display connector", "display.kicad_sch")
s.text((20, 18), "20-way ribbon to display board: 4.3in TFT (RA8875, SPI),"
                 " 74HC595 LEDs, 74HC165 keys. Backlight ~250mA on +5V.", size=2.0)

j12 = s.place("Connector_Generic:Conn_02x10_Odd_Even", "J12", "Ribbon 20-way", (120, 90),
              ref_at=(112, 74), val_at=(124, 74))
RIB = {
    1: ("+5V", "pwr"), 2: ("+5V", "pwr"),
    3: ("GND", "pwr"), 4: ("GND", "pwr"),
    5: ("+3V3", "pwr"), 6: ("+3V3", "pwr"),
    7: ("SPI1_SCK", "output"), 8: ("GND", "pwr"),
    9: ("SPI1_MOSI", "output"), 10: ("SPI1_MISO", "input"),
    11: ("GND", "pwr"), 12: ("TFT_CS", "output"),
    13: ("LED_CS", "output"), 14: ("KEY_CS", "output"),
    15: ("TFT_RST", "output"), 16: ("TFT_INT", "input"),
    17: ("BL_PWM", "output"), 18: ("GND", "pwr"),
    19: ("GND", "pwr"), 20: ("GND", "pwr"),
}
for num, (net, shape) in RIB.items():
    xy = j12.pin(num)
    left_side = num % 2 == 1
    dx = -6 if left_side else 6
    s.wire(xy, (xy[0] + dx, xy[1]))
    if net in ("+5V", "+3V3"):
        rail(s, net, (xy[0] + dx, xy[1]))
    elif net == "GND":
        gnd(s, (xy[0] + dx, xy[1]))
    else:
        s.glabel((xy[0] + dx, xy[1]), net, rot=(180 if left_side else 0), shape=shape)
s.text((90, 130), "Display board itself is a separate KiCad project (phase 2b).", size=1.5)

# =================================================================
# Footprint assignment (Phase 3)
# =================================================================
# By-refdes exceptions first, then value rules, then per-symbol defaults.
# Every name verified to exist in the KiCad 7 standard libraries.

_MKDS15 = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-{n}-5.08_1x{n:02d}_P5.08mm_Horizontal"
_MKDS3 = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-3-{n}-5.08_1x{n:02d}_P5.08mm_Horizontal"
_SOT23 = "Package_TO_SOT_SMD:SOT-23"
_SOIC8 = "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"

FP_BY_REF = {
    # power chain
    "F1":  "Fuse:Fuseholder_Blade_Mini_Keystone_3568",
    "F2":  "Fuse:Fuse_Bourns_MF-RG900",                    # RXEF110 radial PPTC:
                                                       # needs a 1.01mm drill for
                                                       # its 0.81mm leads and a
                                                       # 12.8mm body envelope
    "D1":  "Diode_SMD:D_SMC",                              # SMCJ33CA
    "D2":  "Diode_SMD:D_SOD-123",                          # BZT52C15
    "Q1":  "Package_TO_SOT_SMD:TO-252-2",                  # SQD50P06 DPAK: 1=G, 2=D(tab), 3=S
    "C1":  "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm",       # 100uF 35V
    "C9":  "Capacitor_THT:CP_Radial_D12.5mm_P5.00mm",      # 2200uF 25V crank
                                                           # hold-up bank
    "D3":  "Diode_SMD:D_SMA",                              # SS34 blocking
    "U1":  "Package_SO:SOIC-8-1EP_3.9x4.9mm_P1.27mm_EP2.514x3.2mm_ThermalVias",  # LM5164 DDA (HSOP-8)
    "L2":  "Inductor_SMD:L_12x12mm_H8mm",                  # 33uH 2A shielded (e.g. WE-PD 1245)
    "C5":  "Capacitor_SMD:C_1210_3225Metric",              # 22uF 25V
    "C6":  "Capacitor_SMD:C_1210_3225Metric",
    "C3":  "Capacitor_SMD:C_1210_3225Metric",              # 2.2uF 100V
    "U2":  "Package_TO_SOT_SMD:SOT-23-5",                  # TLV75533PDBV
    "C8":  "Capacitor_SMD:C_0805_2012Metric",              # 10uF
    # MCU core
    "U12": "Package_QFP:LQFP-100_14x14mm_P0.5mm",
    "Y1":  "Crystal:Crystal_SMD_HC49-SD",
    "Y2":  "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm",
    "C76": "Capacitor_THT:CP_Radial_D10.0mm_P5.00mm",      # 0.22F 5.5V EDLC coin
    "C77": "Capacitor_SMD:CP_Elec_4x5.4",                  # 4.7uF SMD electrolytic
    "D70": "Diode_SMD:D_SOD-123",                          # BAT54 variant (SOD-123, e.g. BAT54GWX)
    "C30": "Capacitor_SMD:C_0805_2012Metric",              # 100nF 100V (MPU coupling)
    "DS1": "LED_SMD:LED_0805_2012Metric",
    # analog / misc power parts
    "R88": "Resistor_THT:R_Axial_Power_L20.0mm_W6.4mm_P25.40mm",  # 120R 3W axial
    "C91": "Capacitor_SMD:C_0805_2012Metric",              # 10uF VREF_MID
    # connectors — field power/AC/relay/CT on MKDS-3 (heavier, 400V-class),
    # signal-level on MKDS-1,5
    "J1":  _MKDS3.format(n=2),
    "J2":  _MKDS15.format(n=10),
    "J3":  _MKDS15.format(n=4),
    "J4":  _MKDS15.format(n=2),
    # J5/J6/J15 carry 415V-class field wiring: 8-pole 5.08mm blocks with only
    # the ODD poles wired (pad remap in gen_pcb.py) -> 10.16mm live-to-live
    # pitch, which meets PD2 creepage where a fully-populated 5.08mm block
    # (400V class) does not (SME review catch)
    "J5":  _MKDS3.format(n=8),
    "J6":  _MKDS3.format(n=8),
    "J7":  _MKDS3.format(n=6),
    # no 5-pole MKDS exists; a 6-pole body carries the 5 wired poles and
    # leaves pole 6 spare
    "J8":  _MKDS3.format(n=6),
    "J15": _MKDS3.format(n=8),
    "J9":  _MKDS15.format(n=3),
    "J10": _MKDS15.format(n=3),
    "J11": "Connector:Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical",
    "J12": "Connector_IDC:IDC-Header_2x10-1MP_P2.54mm_Latch_Vertical",
    "J13": _MKDS15.format(n=2),
    "J14": "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical",
}

FP_BY_LIBID = {
    "Device:R": "Resistor_SMD:R_0603_1608Metric",
    "Device:C": "Capacitor_SMD:C_0603_1608Metric",
    "Device:D_Schottky": "Diode_SMD:D_SMA",                # SS34 flybacks
    "Device:D_TVS": "Diode_SMD:D_SMB",                     # SMBJ… (D1 overridden)
    "Device:D_Zener": "Diode_SMD:D_SOD-123",
    "Device:FerriteBead": "Inductor_SMD:L_0603_1608Metric",
    "Diode:BAT54S": _SOT23,
    "Diode:BAV99": _SOT23,
    "Transistor_BJT:BC857": _SOT23,
    "Transistor_FET:2N7002K": _SOT23,
    "Amplifier_Operational:LM358": _SOIC8,
    "Amplifier_Operational:MCP6002-xSN": _SOIC8,
    "Comparator:LM2903": _SOIC8,
    "Interface_CAN_LIN:TJA1051T-3": _SOIC8,
    "Interface_UART:MAX3485": _SOIC8,                      # THVD1450, SOIC-8
    "Memory_EEPROM:M95256-WMN6P": _SOIC8,                  # M95M02-DR MN
    "Relay:G5LE-1": "Relay_THT:Relay_SPDT_Omron-G5LE-1",
    "Jumper:SolderJumper_2_Open": "Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm",
    "Connector:Conn_ARM_SWD_TagConnect_TC2030":
        "Connector:Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical",
    "Connector_Generic:Conn_02x10_Odd_Even":
        "Connector_IDC:IDC-Header_2x10-1MP_P2.54mm_Latch_Vertical",
}


def footprint_for(ref, value, lib_id):
    if ref in FP_BY_REF:
        return FP_BY_REF[ref]
    if lib_id == "Device:R":
        if "3W" in value:
            return "Resistor_SMD:R_2512_6332Metric"        # 0.05R shunt, 3W-class 2512
        if "0.5W" in value:
            return "Resistor_SMD:R_1210_3225Metric"        # DIN divider top, surge margin
        if "3W" in value:
            return "Resistor_THT:R_Axial_Power_L20.0mm_W6.4mm_P25.40mm"
        if "1206" in value:
            return "Resistor_SMD:R_1206_3216Metric"        # AC-sense chain, 200V/element
    if lib_id == "Device:C" and "100V" in value:
        return "Capacitor_SMD:C_0805_2012Metric"
    if lib_id == "Device:FerriteBead" and "3A" in value:
        return "Inductor_SMD:L_0805_2012Metric"            # FB1, series element in +12V feed
    return FP_BY_LIBID.get(lib_id)


for _sh in [p.root] + [s0 for s0, _ in p.sheets]:
    for _sym in _sh.sch.schematicSymbols:
        _props = {pr.key: pr for pr in _sym.properties}
        _ref = _props["Reference"].value
        if _ref.startswith("#") or _sym.libId.startswith("power:"):
            continue
        _val = _props["Value"].value
        _fp = footprint_for(_ref, _val, _sym.libId)
        if not _fp:
            raise SystemExit(f"no footprint rule for {_ref} ({_sym.libId} / {_val})")
        _props["Footprint"].value = _fp

# =================================================================
p.save(OUT)
print("Generated", os.path.abspath(OUT))
