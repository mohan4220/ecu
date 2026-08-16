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
    # top edge
    "F1":  (46, 30, 0),       # blade fuse holder
    "J12": (110, 31, 90),     # display ribbon, horizontal along top edge
    "J5":  (152, 29, 0),      # GEN L1-L3+N
    "J6":  (178, 29, 0),      # MAINS L1-L3+N
    # right edge (pads run +y at rot 270)
    "J7":  (211, 44, 270),    # CT 3 pairs
    # bottom edge (pads run -x from anchor at rot 180)
    "J2":  (85, 161, 180),    # DIN x8 + wetting
    "J3":  (112, 161, 180),   # senders
    "J9":  (132, 161, 180),   # CAN
    "J10": (152, 161, 180),   # RS485
    "J8":  (204, 161, 180),   # relay contacts
    # interior anchors
    "U12": (112, 88, 0),      # STM32F407 LQFP-100, board center
    "J11": (128, 106, 0),     # TC2030 SWD pads, next to MCU
    "J14": (100, 140, 0),     # debug UART header
    "K1":  (151, 104, 0), "K2": (174, 104, 0), "K3": (197, 104, 0),
    "K4":  (151, 131, 0), "K5": (174, 131, 0), "K6": (197, 131, 0),
}

# shelf-pack zones per schematic sheet: (x0, y0, x1, y1)
ZONES = {
    "Power":                (36, 26, 96, 62),
    "Analog inputs":        (36, 64, 92, 98),
    "Digital inputs + MPU": (36, 100, 92, 154),
    "MCU":                  (96, 68, 136, 112),
    "AC sensing":           (138, 38, 208, 92),
    "Comms":                (96, 118, 126, 154),
    "Relay drivers":        (129, 96, 141, 154),
    "Display connector":    (96, 40, 136, 64),   # J12 is fixed; spares land here
}


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
    board.GetDesignSettings().SetCopperLayerCount(2)

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
            by_zone.setdefault(sheet, []).append(ref)

    for sheet, refs in by_zone.items():
        zx0, zy0, zx1, zy1 = ZONES[sheet]
        # big parts first: fewer awkward gaps
        refs.sort(key=lambda r: -(lambda bb: (bb[2]-bb[0])*(bb[3]-bb[1]))(fp_rect(fps[r])))
        cx, cy, rowh = zx0, zy0, 0.0
        for ref in refs:
            fp = fps[ref]
            bb = fp_rect(fp, margin=0.4)
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
            hit = 0
            for pad in fps[ref].Pads():
                if pad.GetNumber() == pin:
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

    # GND pour, both layers
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(netinfo["GND"])
        z.SetMinThickness(MM(0.25))
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
    # HV classes derived from the netlist: nets on J5/J6 terminals carry full
    # 415 V line; interior nodes of the 4x330k divider chains sit at stepped
    # potentials (>=85 Vpk) — both get widened clearance. The chain-bottom
    # node (also touches a 5.62k) is /237 and stays at default clearance.
    r330 = {r for r, (v, _, _) in comps.items() if v == "330k 1206"}
    r562 = {r for r, (v, _, _) in comps.items() if v.startswith("5.62k")}
    hv_in, hv_mid = [], []
    for name, nodes in nets.items():
        refs = {r for r, _ in nodes}
        if refs & {"J5", "J6"}:
            hv_in.append(name)
        elif (refs & r330) and not (refs & r562):
            hv_mid.append(name)

    def cond(netnames):
        return " || ".join(f"A.NetName == '{n}'" for n in sorted(netnames))

    dru = f"""(version 1)

# 415 V AC sense inputs (J5/J6 terminals up to the first 330k): creepage-class
(rule ac_line_clearance
  (condition "{cond(hv_in)}")
  (constraint clearance (min 2.5mm)))

# interior nodes of the 4x330k chains (~85..255 Vpk)
(rule ac_chain_clearance
  (condition "{cond(hv_mid)}")
  (constraint clearance (min 1.0mm)))
"""
    with open(f"{OUT}/ecu25-main.kicad_dru", "w") as f:
        f.write(dru)
    print(f"wrote ecu25-main.kicad_dru  ({len(hv_in)} line nets, {len(hv_mid)} chain nets)")


if __name__ == "__main__":
    main()
