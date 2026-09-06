"""
gen_pcb.py — generates the starting-point PCB for the ECU-25 main board.

Reads the netlist exported from the generated schematic, loads the assigned
footprints from the KiCad 7 standard libraries, and produces
hardware/kicad/ecu25-main/ecu25-main.kicad_pcb with:

  - 200 x 150 mm 2-layer outline, M3 mounting holes in the corners
  - all 299 footprints placed: field connectors on the edges, big parts at
    fixed positions, everything else shelf-packed inside its subsystem zone
  - every pad on its net (ratsnest complete)
  - GND pour on both copper layers
  - ecu25-main.kicad_dru with clearance rules for the 415 V AC sense chain

Placement is a grouped starting point, not a routed layout: tracks are done
interactively in pcbnew (or freerouting) after ERC/DRC.

Regenerate:
  kicad-cli sch export netlist --output /tmp/ecu25.net \
      hardware/kicad/ecu25-main/ecu25-main.kicad_sch
  python3 hardware/kicad/gen/gen_pcb.py /tmp/ecu25.net
"""

import sys, os, re
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "ecu25-main")
FPLIB = "/usr/share/kicad/footprints"

MM = pcbnew.FromMM


def V(x, y):
    return pcbnew.VECTOR2I(MM(x), MM(y))


# ------------------------------------------------------------ netlist parse

def parse_netlist(path):
    s = open(path).read()
    comps = {}
    for m in re.finditer(
            r'\(comp \(ref "([^"]+)"\)\s*\(value "([^"]*)"\)\s*'
            r'\(footprint "([^"]*)"\)(.*?)(?=\(comp |\(libparts)', s, re.S):
        ref, val, fp, rest = m.groups()
        sheet = re.search(r'\(property \(name "Sheetname"\) \(value "([^"]*)"\)', rest)
        comps[ref] = (val, fp, sheet.group(1) if sheet else "")
    nets = {}
    for m in re.finditer(r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)(?=\(net |\Z)', s, re.S):
        nets[m.group(1)] = re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', m.group(2))
    return comps, nets


# ------------------------------------------------------------ board layout

# outline
X0, Y0, X1, Y1 = 20.0, 20.0, 220.0, 170.0
HOLES = [(26, 26), (214, 26), (26, 164), (214, 164)]

# fixed placements: ref -> (x, y, rot). Anchor is the footprint origin
# (pad 1 for terminal blocks and relays). Terminal-block wire entry faces:
# rot 0 = -y (top edge), 180 = +y (bottom), 90 = -x (left), 270 = +x (right).
FIXED = {
    # left edge (pads run -y from anchor at rot 90)
    "J1":  (29, 48, 90),      # battery in
    "J4":  (29, 68, 90),      # MPU pickup
    "J13": (29, 84, 90),      # charge alternator D+
    "J3":  (29, 108, 90),     # senders (moved off the bottom edge for J15)
    "R88": (30, 150, 90),     # 220R 5W: near the edge, away from precision analog
    # top edge: power chain then AC blocks. J5/J6/J15 are 8-pole bodies with
    # only odd poles wired (10.16mm live pitch, see PAD_REMAP)
    "F1":  (46, 30, 0),       # blade fuse holder
    "J12": (100, 35, 0),      # display ribbon, vertical, cable exits upward
    "J5":  (110, 29, 0),      # GEN L1-L3+N on poles 1/3/5/7
    "J6":  (158, 29, 0),      # MAINS L1-L3+N on poles 1/3/5/7
    # right edge (pads run +y at rot 270)
    "J7":  (211, 44, 270),    # CT 3 pairs
    # CT front-end lives at its terminal: TVS protects the pluggable
    # connector, burden loop stays small (SME review)
    "D50": (202, 47, 90), "D51": (202, 57, 90), "D52": (202, 67, 90),
    "R50": (196, 47, 90), "R55": (196, 57, 90), "R59": (196, 67, 90),
    # bottom edge (pads run -x from anchor at rot 180)
    "J2":  (85, 161, 180),    # DIN x8 + wetting
    "J9":  (108, 161, 180),   # CAN
    "J10": (126, 161, 180),   # RS485
    "J8":  (160, 161, 180),   # OUT_COM + 4 dry NO outputs (6-pole body)
    "J15": (204, 161, 180),   # volt-free contactor pairs, odd poles
    # power chain in electrical order, buck hot loop tight (SME review)
    "D1":  (65, 31, 0),       # input TVS
    "Q1":  (78, 31, 0),       # reverse-polarity P-FET
    "FB1": (85, 28, 0),
    "C1":  (44, 57, 0),         # bulk 100uF radial — the only gap wide
                            # enough left of J12
    "C9":  (60, 62, 0),         # 2200uF crank hold-up: too big to shelf-pack,
                            # sits below L2 and right of C1
    "C3":  (46, 46, 90),      # buck VIN cap at U1
    "U1":  (56, 46, 0),       # LM5164
    "L2":  (72, 48, 0),
    "C5":  (84, 43, 90), "C6": (84, 51, 90),
    # interior anchors
    "U12": (112, 88, 0),      # STM32F407 LQFP-100, board center
    "J11": (128, 106, 0),     # TC2030 SWD pads, next to MCU
    "J14": (100, 140, 0),     # debug UART header
    # relays: GEN/MAINS (volt-free, K3/K4) share the middle column above
    # J15's poles; their contact corridor gets a pour keepout
    "K1":  (148, 104, 0), "K3": (169, 104, 0), "K2": (190, 104, 0),
    "K5":  (148, 131, 0), "K4": (169, 131, 0), "K6": (190, 131, 0),
}

# J5/J6/J15: symbol pin n -> footprint pad 2n-1 (odd poles of the 8-pole block).
# WARNING: this remap exists ONLY here. Running "Update PCB from Schematic" in
# pcbnew would silently move these nets back to pads 1-4 (2.48mm live pitch,
# creepage violation). This board is regenerate-only — see README.
PAD_REMAP = {r: {"1": "1", "2": "3", "3": "5", "4": "7"} for r in ("J5", "J6", "J15")}

# shelf-pack zones per schematic sheet: (x0, y0, x1, y1)
ZONES = {
    "Power":                (36, 26, 96, 62),
    "Analog inputs":        (36, 64, 92, 98),
    "Digital inputs + MPU": (36, 100, 92, 154),
    "MCU":                  (96, 58, 138, 116),
    "AC-HV":                (108, 38, 190, 56),  # 330k divider chains only
    "AC sensing":           (138, 60, 205, 92),  # low-level AC/CT/VREF parts
    "Comms":                (96, 118, 125, 154),
    "Relay drivers":        (126, 94, 144, 155),
    "Display connector":    (96, 40, 136, 64),   # J12 is fixed; spares land here
}

# copper-pour keepouts (both layers): under the 415V terminals, under the
# divider chains, and the volt-free contact corridor K3/K4 -> J15
KEEPOUTS = [
    (103, 20, 206, 37),     # J5/J6 bodies
    (103, 37, 192, 58),     # 330k chain strip
    (155, 96, 209, 166),    # volt-free relay contacts + corridor + J15
]


def load_fp(fpid):
    lib, name = fpid.split(":")
    fp = pcbnew.FootprintLoad(f"{FPLIB}/{lib}.pretty", name)
    if fp is None:
        raise SystemExit(f"footprint not found: {fpid}")
    return fp


def fp_rect(fp, margin=0.0):
    bb = fp.GetBoundingBox(False, False)
    return (bb.GetLeft() / 1e6 - margin, bb.GetTop() / 1e6 - margin,
            bb.GetRight() / 1e6 + margin, bb.GetBottom() / 1e6 + margin)


def overlaps(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])


def main():
    netfile = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ecu25.net"
    comps, nets = parse_netlist(netfile)
    print(f"{len(comps)} components, {len(nets)} nets")

    pcb_path = os.path.abspath(f"{OUT}/ecu25-main.kicad_pcb")
    board = pcbnew.NewBoard(pcb_path)
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(2)
    # must match the Default netclass written into .kicad_pro below: the zone
    # fill happens here, before KiCad ever reads that file, so a mismatch
    # leaves the pour too close to every pad and DRC lights up. 0.15mm is the
    # ceiling: an LQFP-100 at 0.5mm pitch has only 0.2mm between its own pads.
    ds.m_MinClearance = MM(0.15)
    # U1's library footprint carries 0.2mm thermal vias under the exposed pad;
    # that is standard for a power IC and every fab does it, but it sits under
    # KiCad's 0.3mm default minimum, which then reports each one as an error.
    ds.m_MinThroughDrill = MM(0.2)

    # outline
    rect = pcbnew.PCB_SHAPE(board)
    rect.SetShape(pcbnew.SHAPE_T_RECT)
    rect.SetStart(V(X0, Y0))
    rect.SetEnd(V(X1, Y1))
    rect.SetLayer(pcbnew.Edge_Cuts)
    rect.SetWidth(MM(0.1))
    rect.SetFilled(False)
    board.Add(rect)

    # net objects
    netinfo = {}
    for name in nets:
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        netinfo[name] = ni

    # mounting holes
    for i, (hx, hy) in enumerate(HOLES, 1):
        fp = load_fp("MountingHole:MountingHole_3.2mm_M3")
        fp.SetReference(f"H{i}")
        fp.Reference().SetVisible(False)
        fp.SetPosition(V(hx, hy))
        board.Add(fp)

    # footprints: fixed first (collect their rects), then shelf-pack per zone
    placed_rects = []   # global fixed/packed rects for collision checks
    by_zone = {}
    fps = {}
    for ref, (val, fpid, sheet) in sorted(comps.items()):
        fp = load_fp(fpid)
        fp.SetReference(ref)
        fp.SetValue(val)
        fps[ref] = fp
        board.Add(fp)
        if ref in FIXED:
            x, y, rot = FIXED[ref]
            fp.SetOrientationDegrees(rot)
            fp.SetPosition(V(x, y))
            placed_rects.append(fp_rect(fp, margin=0.5))
        else:
            zone = "AC-HV" if val == "330k 1206" else sheet
            by_zone.setdefault(zone, []).append(ref)

    for sheet, refs in by_zone.items():
        zx0, zy0, zx1, zy1 = ZONES[sheet]
        # wider spacing in the HV strip so pad-to-pad gaps clear the 1mm
        # chain clearance rule; refs sorted so chain resistors stay grouped
        margin = 1.3 if sheet == "AC-HV" else 0.6   # 1.3 -> chain pad gaps clear 3.0mm
        if sheet == "AC-HV":
            refs.sort()
        else:
            # big parts first: fewer awkward gaps
            refs.sort(key=lambda r: -(lambda bb: (bb[2]-bb[0])*(bb[3]-bb[1]))(fp_rect(fps[r])))
        cx, cy, rowh = zx0, zy0, 0.0
        for ref in refs:
            fp = fps[ref]
            bb = fp_rect(fp, margin=margin)
            w, h = bb[2] - bb[0], bb[3] - bb[1]
            while True:
                if cx + w > zx1:                    # wrap row; if the whole
                    # row was blocked (rowh still 0) creep down instead of
                    # spinning forever on the same y
                    cx, cy, rowh = zx0, cy + (rowh or 1.0), 0.0
                if cy + h > zy1:
                    raise SystemExit(f"zone '{sheet}' overflow at {ref}")
                cand = (cx, cy, cx + w, cy + h)
                hit = next((r for r in placed_rects if overlaps(cand, r)), None)
                if hit is None:
                    break
                cx = hit[2] + 0.2                   # skip past the blocker
            # anchor = cursor - bbox offset
            fp.SetPosition(V(cx - bb[0], cy - bb[1]))
            placed_rects.append(cand)
            cx += w
            rowh = max(rowh, h)

    # nets onto pads (a pad number can appear on several physical pads, e.g.
    # the DPAK tab — all of them get the net)
    missing = []
    for name, nodes in nets.items():
        for ref, pin in nodes:
            padnum = PAD_REMAP.get(ref, {}).get(pin, pin)
            hit = 0
            for pad in fps[ref].Pads():
                if pad.GetNumber() == padnum:
                    pad.SetNet(netinfo[name])
                    hit += 1
            if not hit:
                missing.append((ref, pin))
    if missing:
        raise SystemExit(f"pads not found for nodes: {missing}")

    # ribbon-header mounting lugs onto GND (mechanical pads, not in netlist)
    for pad in fps["J12"].Pads():
        if pad.GetNumber() == "MP":
            pad.SetNet(netinfo["GND"])

    # pour keepouts first (rule areas, both layers)
    for kx0, ky0, kx1, ky1 in KEEPOUTS:
        z = pcbnew.ZONE(board)
        z.SetIsRuleArea(True)
        z.SetDoNotAllowCopperPour(True)
        z.SetDoNotAllowTracks(False)
        z.SetDoNotAllowVias(False)
        z.SetDoNotAllowPads(False)      # default True floods DRC with errors
        z.SetDoNotAllowFootprints(False)
        ls = pcbnew.LSET()
        ls.AddLayer(pcbnew.F_Cu)
        ls.AddLayer(pcbnew.B_Cu)
        z.SetLayerSet(ls)
        o = z.Outline()
        o.NewOutline()
        for x, y in ((kx0, ky0), (kx1, ky0), (kx1, ky1), (kx0, ky1)):
            o.Append(MM(x), MM(y))
        board.Add(z)

    # pour keepouts around every non-plated hole: the fill otherwise runs into
    # the drill wall (0.00mm) and DRC flags each one as a hole-clearance error
    npth = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                pos = pad.GetPosition()
                r = max(pad.GetDrillSize().x, pad.GetDrillSize().y) / 2e6 + 0.6
                npth.append((pos.x / 1e6, pos.y / 1e6, r))
    for hx, hy, r in npth:
        z = pcbnew.ZONE(board)
        z.SetIsRuleArea(True)
        z.SetDoNotAllowCopperPour(True)
        z.SetDoNotAllowTracks(False)
        z.SetDoNotAllowVias(False)
        z.SetDoNotAllowPads(False)
        z.SetDoNotAllowFootprints(False)
        ls = pcbnew.LSET()
        ls.AddLayer(pcbnew.F_Cu)
        ls.AddLayer(pcbnew.B_Cu)
        z.SetLayerSet(ls)
        o = z.Outline()
        o.NewOutline()
        for dx, dy in ((-r, -r), (r, -r), (r, r), (-r, r)):
            o.Append(MM(hx + dx), MM(hy + dy))
        board.Add(z)
    print(f"  {len(npth)} NPTH pour keepouts")

    # GND pour, both layers
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(netinfo["GND"])
        z.SetMinThickness(MM(0.25))
        # the script-time fill cannot see the netclasses (they are written to
        # .kicad_pro below, and KiCad reads that later), so pour at the widest
        # non-HV class — Battery 0.4mm. The HV nets are covered by KEEPOUTS.
        z.SetLocalClearance(MM(0.4))
        o = z.Outline()
        o.NewOutline()
        for x, y in ((X0+0.5, Y0+0.5), (X1-0.5, Y0+0.5), (X1-0.5, Y1-0.5), (X0+0.5, Y1-0.5)):
            o.Append(MM(x), MM(y))
        board.Add(z)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())

    # silkscreen title
    t = pcbnew.PCB_TEXT(board)
    t.SetText("ECU-25 main  rev A")
    t.SetPosition(V(70, 22.8))
    t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(V(3, 3))
    board.Add(t)

    pcbnew.SaveBoard(pcb_path, board)
    print("wrote", pcb_path)

    # ------------------------------------------------- custom DRC rules
    # HV classes derived from the netlist:
    #  - line nets: J5/J6 pins 1-3 (415 V L-L between GEN and MAINS phases,
    #    up to ~680 Vpk between unsynchronized sources) and the volt-free
    #    contactor nets on J15 (independent AC sources) -> 3.0 mm clearance.
    #    Pin 4 (N) lands on GND and stays in the default class.
    #  - chain nets: interior nodes of the 4x330k dividers (85..255 Vpk)
    #    -> 1.0 mm. The chain-bottom node (touches a 5.62k) is /237, default.
    #  - each line net is exempted down to 1.0 mm against its OWN first chain
    #    node (the two pads of the first 330k are 1.8 mm apart) — KiCad
    #    applies the LAST matching rule, so exemptions come after the class.
    r330 = {r for r, (v, _, _) in comps.items() if v == "330k 1206"}
    r562 = {r for r, (v, _, _) in comps.items() if v.startswith("5.62k")}
    net_of = {}
    for name, nodes in nets.items():
        for ref, pin in nodes:
            net_of[(ref, pin)] = name
    hv_line = set()
    for j in ("J5", "J6"):
        for pin in ("1", "2", "3"):
            n = net_of.get((j, pin))
            if n and n != "GND":
                hv_line.add(n)
    for pin in ("1", "2", "3", "4"):
        n = net_of.get(("J15", pin))
        if n:
            hv_line.add(n)
    hv_mid = set()
    for name, nodes in nets.items():
        refs = {r for r, _ in nodes}
        if name not in hv_line and (refs & r330) and not (refs & r562):
            hv_mid.add(name)
    # line net -> its own first interior node (other net of the shared 330k)
    own_pairs = []
    for line in hv_line:
        for ref, pin in nets[line]:
            if ref in r330:
                other = net_of[(ref, "2" if pin == "1" else "1")]
                if other in hv_mid:
                    own_pairs.append((line, other))

    def cond(netnames):
        return " || ".join(f"A.NetName == '{n}'" for n in sorted(netnames))

    parts = [f"""(version 1)

# GENERATED by hardware/kicad/gen/gen_pcb.py — do not hand-edit; net names
# here go stale if the schematic is re-annotated. Regenerate instead.

# interior nodes of the 4x330k sense chains (~85..255 Vpk)
(rule ac_chain_clearance
  (condition "{cond(hv_mid)}")
  (constraint clearance (min 1.0mm)))

# 415 V field wiring: GEN/MAINS phases (J5/J6) and volt-free contactor
# circuits (J15) — includes clearance to GND and to each other
(rule ac_line_clearance
  (condition "{cond(hv_line)}")
  (constraint clearance (min 3.0mm)))

# unwired pads (empty even poles of J5/J6/J15, mech pads): default clearance
(rule netless_pads
  (condition "A.NetName == ''")
  (constraint clearance (min 0.2mm)))
"""]
    for i, (line, node) in enumerate(sorted(own_pairs), 1):
        parts.append(f"""
# a line net may sit 1.0mm from its OWN first divider node (same chain)
(rule ac_own_chain_{i}
  (condition "A.NetName == '{line}' && B.NetName == '{node}'")
  (constraint clearance (min 1.0mm)))
""")
    with open(f"{OUT}/ecu25-main.kicad_dru", "w") as f:
        f.write("".join(parts))
    print(f"wrote ecu25-main.kicad_dru  ({len(hv_line)} line nets, "
          f"{len(hv_mid)} chain nets, {len(own_pairs)} own-chain exemptions)")

    # -------------------------------------------- netclasses in .kicad_pro
    # Track widths for routing (and DSN export -> freerouting). Only the
    # net_settings key is rewritten; the rest of the user's project file is
    # preserved.
    import json
    battery = {"/Power/VBAT_IN", "Net-(D1-A2)", "Net-(D2-K)", "+24V"}
    # logic rails need current-carrying width but reach fine-pitch parts, so
    # they cannot take a wide clearance: the LQFP-100's own pads are 0.2mm
    # apart, and +3V3/+5V land on them (BOM review catch).
    logic = {"+5V", "+3V3"}
    power = {"+24V_SW", "Net-(U1-SW)", "Net-(J13-Pin_1)"}
    power |= {n for n in nets
              if n.endswith(("FUEL_OUT", "START_OUT", "HORN_OUT", "PREHEAT_OUT"))}
    power |= {n for n, nodes in nets.items()
              if any(r.startswith("K") and r[1:].isdigit() and p == "2"
                     for r, p in nodes)}                     # relay coil drains
    power |= hv_line                                         # contactor pairs

    def klass(name, track, clearance=0.25, via=0.8, drill=0.4):
        return {"bus_width": 12, "clearance": clearance, "diff_pair_gap": 0.25,
                "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2,
                "line_style": 0, "microvia_diameter": 0.3,
                "microvia_drill": 0.1, "name": name,
                "pcb_color": "rgba(0, 0, 0, 0.000)",
                "schematic_color": "rgba(0, 0, 0, 0.000)",
                "track_width": track, "via_diameter": via, "via_drill": drill,
                "wire_width": 6}

    pro_path = f"{OUT}/ecu25-main.kicad_pro"
    pro = json.load(open(pro_path))
    pro["net_settings"] = {
        "classes": [
            # Default clearance is bounded by the LQFP-100's own 0.2mm
            # pad-to-pad gap; wider spacing lives in the power classes.
            klass("Default", 0.25, clearance=0.15),
            klass("GND", 0.5, clearance=0.2),
            klass("Power", 1.0, clearance=0.3, via=1.0, drill=0.5),
            klass("Logic", 0.6, clearance=0.15, via=0.8, drill=0.4),
            klass("Battery", 2.0, clearance=0.4, via=1.2, drill=0.6),
            # HV clearances also live in the .kicad_dru (with per-chain
            # exemptions); duplicating them as netclasses lets the DSN
            # export carry them into freerouting
            klass("HV_AC", 0.5, clearance=3.0),
            klass("HV_CHAIN", 0.25, clearance=1.0),
        ],
        "meta": {"version": 3},
        "net_colors": None,
        "netclass_assignments": None,
        "netclass_patterns":
            [{"netclass": "Battery", "pattern": n} for n in sorted(battery)] +
            [{"netclass": "Power", "pattern": n} for n in sorted(power - hv_line)] +
            [{"netclass": "Logic", "pattern": n} for n in sorted(logic)] +
            [{"netclass": "HV_AC", "pattern": n} for n in sorted(hv_line)] +
            [{"netclass": "HV_CHAIN", "pattern": n} for n in sorted(hv_mid)] +
            [{"netclass": "GND", "pattern": "GND"}],
    }
    with open(pro_path, "w") as f:
        json.dump(pro, f, indent=2)
    print(f"updated net_settings in ecu25-main.kicad_pro "
          f"({len(battery)} battery, {len(power - hv_line)} power, "
          f"{len(logic)} logic nets)")


if __name__ == "__main__":
    main()
