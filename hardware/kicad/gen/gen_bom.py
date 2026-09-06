"""
gen_bom.py — builds the ECU-25 bill of materials from the schematic netlist.

Outputs:
  docs/bom/ecu25-main-bom.csv   production BOM, one line per value+footprint
  docs/bom/ecu25-main-bom.md    same grouped by subsystem, human-readable

Part numbers come from PARTS below (actives and anything where the value
string alone is not orderable); passives are ordered by value + package +
the rating column, which is what the calc sheet in docs/circuits/README.md
specifies.

Usage:
  kicad-cli sch export netlist hardware/kicad/ecu25-main/ecu25-main.kicad_sch -o /tmp/ecu25.net
  python3 hardware/kicad/gen/gen_bom.py /tmp/ecu25.net
"""

import sys, os, re, csv, collections

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.path.join(HERE, "..", "..", "..", "docs", "bom"))

# ref-prefix -> ordering hint for passives that need more than the value
RATING = {
    "330k 1206": "200 V element rating, 1 % — AC divider chain",
    "5.6k 0.5W": "0.5 W / 1210 — DIN divider top leg, sized for 1.6 mA wetting at 12 V",
    "120R 3W": "3 W axial — D+ excitation, ~1.2 W continuous in a charge-fail",
    "1uF 50V": "DIN debounce, tau approximately 1.4 ms with the 5.6k/1.8k divider",
    "0.05R 3W": "3 W 2512 current-sense (Kelvin pads), 1 %",
    "100nF 100V": "X7R 100 V — MPU AC coupling",
    "2.2uF 50V": "X7R 50 V — buck input bypass",
    "49.9k 1%": "buck RON: sets fsw ≈ 300 kHz from a 12 V input",
    "30.1k 1%": "buck UVLO: turn-on ≈ 6.5 V, low enough to ride a 12 V crank dip",
    "22nF C0G": "C0G/NP0 — anti-alias filters, needs low drift",
    "12pF": "C0G — HSE load caps",
    "6.8pF": "C0G — LSE load caps",
    "6.8k 1%": "1 % — phase-matches the AC voltage channel; PF accuracy depends on it",
    "10k 1%": "1 % — CT gain pairs (1 + R52/R51 = 2) and the VREF divider",
    "1M": "MPU comparator hysteresis, ±79 mV with the 100 k bias pair",
}

# refdes -> (manufacturer part, note)
PARTS = {
    "U1":  ("LM5164DDA-Q1", "100 V synchronous buck, HSOP-8 with thermal pad"),
    "U2":  ("TLV75533PDBVR", "3.3 V 500 mA LDO, SOT-23-5"),
    "U3":  ("LM358DR", "sender current sources 1-2"),
    "U4":  ("LM358DR", "sender current source 3 + spare"),
    "U5":  ("LM2903DR", "MPU comparator"),
    "U6":  ("TJA1051T/3", "CAN transceiver, 3.3 V IO"),
    "U7":  ("THVD1450DR", "RS485, true fail-safe receiver"),
    "U8":  ("MCP6002-I/SN", "VREF_MID buffer"),
    "U9":  ("MCP6002-I/SN", "CT amps 1-2"),
    "U10": ("MCP6002-I/SN", "CT amp 3 + spare"),
    "U11": ("M95M02-DRMN6TP", "2 Mbit SPI EEPROM"),
    "U12": ("STM32F407VGT6", "LQFP-100"),
    "Q1":  ("SQD50P06-15L", "DPAK P-FET, reverse-polarity"),
    "Y1":  ("8 MHz HC-49SD, CL 10 pF", "HSE"),
    "Y2":  ("32.768 kHz 3215, CL 6 pF", "LSE / RTC"),
    "L2":  ("33 uH 2 A shielded, 12x12", "e.g. Wurth WE-PD 7447779233"),
    "F1":  ("Mini blade fuse 5 A + Keystone 3568 holder", ""),
    "F2":  ("RXEF110", "1.1 A hold / 2.2 A trip PPTC, radial. Feeds only the six relay coils (200 mA) and the D+ excitation (100 mA) — field loads are fed by the installer through J8 pin 1, so no field current crosses this board"),
    "C76": ("0.22 F 5.5 V EDLC coin", "RTC backup, radial D10/P5.0"),
    "FB2": ("BLM18PG600SN1", "ferrite bead 0603 — VDDA filter"),
    "C1":  ("100 uF 35 V radial", "input bulk on the battery side of D3"),
    "C9":  ("2200 uF 25 V radial", "crank hold-up: with the backlight shed it carries the logic for >50 ms down to the 6.5 V UVLO, so the MCU does not reset while the starter drags the battery down"),
    "D3":  ("SS34", "blocking Schottky — stops C9 back-feeding the harness during a crank dip"),
    "D1":  ("SMCJ16CA", "bidirectional input TVS, 12 V system — clamps ~26 V"),
    "D2":  ("BZT52C12", "gate zener, holds V_GS inside the P-FET rating"),
    "D70": ("BAT54GWX", "supercap isolation Schottky"),
    "D80": ("SMBJ16CA", "bidirectional, +12V_SW local clamp"),
    "DS1": ("green 0805 LED", "heartbeat"),
    "J11": ("Tag-Connect TC2030-IDC-NL", "footprint only, no fitted part"),
    "J12": ("2x10 IDC header, latched", "display ribbon"),
    "J14": ("1x3 2.54 header", "debug UART"),
}

FAMILY = {
    "BAT54S": ("BAT54S", "dual series Schottky clamp, SOT-23"),
    "BAV199": ("BAV199", "low-leakage dual diode, SOT-23"),
    "BAV99": ("BAV99", "dual switching diode, SOT-23"),
    "SS34": ("SS34", "3 A Schottky flyback, SMA"),
    "SMBJ6.0CA": ("SMBJ6.0CA", "bidirectional TVS at the CT terminals"),
    "2N7002K 60V": ("2N7002K", "60 V logic-level FET, relay driver"),
    "BC857": ("BC857B", "PNP pass device in the sender current source"),
    "G5LE-1 12V": ("G5LE-1-DC12", "Omron SPDT, 12 V coil (360 ohm / 33 mA). Contacts are PILOT DUTY: 8 A DC max, and a 12 V starter solenoid pulls 20-40 A"),
    "600R@100MHz 3A": ("BLM21PG600SN1", "ferrite bead 0805, 3 A — series in the +12 V feed"),
}

SHEET_ORDER = ["Power", "Analog inputs", "AC sensing", "Digital inputs + MPU",
               "Relay drivers", "Comms", "MCU", "Display connector"]


def parse(path):
    s = open(path).read()
    comps = []
    for m in re.finditer(
            r'\(comp \(ref "([^"]+)"\)\s*\(value "([^"]*)"\)\s*'
            r'\(footprint "([^"]*)"\)(.*?)(?=\(comp |\(libparts)', s, re.S):
        ref, val, fp, rest = m.groups()
        sheet = re.search(r'\(property \(name "Sheetname"\) \(value "([^"]*)"\)', rest)
        comps.append((ref, val, fp, sheet.group(1) if sheet else ""))
    return comps


def sortkey(ref):
    m = re.match(r"([A-Z]+)(\d+)", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


def package(fp):
    """Short, orderable package name (the full KiCad footprint id is noise
    in a BOM: what a supplier needs is '0603' or 'SOIC-8')."""
    name = fp.split(":", 1)[1]
    for pat, short in (
            (r"MKDS-1,5-\d+-5\.08_1x(\d+)", r"screw terminal 5.08mm x\1 (signal)"),
            (r"MKDS-3-\d+-5\.08_1x(\d+)", r"screw terminal 5.08mm x\1 (power)"),
            (r"^[RCL]_(\d{4})_", r"\1"),
            (r"^D_(SMA|SMB|SMC)$", r"\1"),
            (r"SOIC-8-1EP", "HSOP-8 (thermal pad)"),
            (r"SOT-23-5", "SOT-23-5"),
            (r"SOT-23", "SOT-23"),
            (r"SOD-123", "SOD-123"),
            (r"SOIC-8", "SOIC-8"),
            (r"LQFP-100", "LQFP-100"),
            (r"TO-252", "DPAK"),
            (r"Fuse_1812", "1812"),
            (r"CP_Radial_D(\d+\.\d)mm_P(\d+\.\d+)mm", r"radial D\1 / P\2"),
            (r"CP_Elec_4x5\.4", "SMD electrolytic 4x5.4"),
            (r"L_12x12mm_H8mm", "12x12x8 shielded"),
            (r"R_Axial_Power_L(\d+\.\d)mm_W(\d+\.\d)mm_P(\d+\.\d+)mm",
             r"axial power L\1 / pitch \3"),
            (r"Crystal_SMD_HC49-SD", "HC-49SD"),
            (r"Crystal_SMD_3215", "3215 SMD"),
            (r"Relay_SPDT_Omron-G5LE-1", "G5LE THT"),
            (r"SolderJumper-2", "solder jumper"),
            (r"IDC-Header_2x10", "IDC 2x10 latched"),
            (r"PinHeader_1x03", "header 1x3 2.54mm"),
            (r"Tag-Connect_TC2030", "TC2030 pads (no part)"),
            (r"Fuseholder_Blade_Mini", "mini blade fuse holder"),
            (r"Fuse_Bourns_MF-RG\d+", "radial PPTC, 5.1mm pitch"),
            (r"RV_Disc_D(\d+)mm_W[\d.]+mm_P(\d+)mm", r"radial disc D\1 / pitch \2"),
            (r"LED_(\d{4})_", r"\1"),
            (r"MountingHole", "M3 hole"),
    ):
        m = re.search(pat, name)
        if m:
            return m.expand(short) if m.groups() else short
    return name


def orderable(val):
    """Strip the schematic's design annotations so identical parts land on one
    order line. '100k (RON 300kHz)' and '100k' are the same thing to buy; the
    annotation survives in the Note column. Explicit ratings (100V, 0.5W, C0G,
    1%) are NOT stripped — those change which part you order."""
    return re.sub(r"\s*\([^)]*\)", "", val).strip()


def main():
    netfile = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ecu25.net"
    comps = parse(netfile)
    os.makedirs(OUT, exist_ok=True)

    groups = collections.OrderedDict()
    for ref, val, fp, sheet in comps:
        key = (orderable(val), package(fp))
        g = groups.setdefault(key, {"refs": [], "sheets": set(), "fp": fp,
                                    "annot": set()})
        g["refs"].append(ref)
        g["sheets"].add(sheet)
        if val != orderable(val):
            g["annot"].add(val)

    rows = []
    for (val, pkg), g in groups.items():
        refs = sorted(g["refs"], key=sortkey)
        part = ""
        note = RATING.get(val, "")
        if len(refs) == 1 and refs[0] in PARTS:
            part, pnote = PARTS[refs[0]]
            note = pnote or note
        elif val in FAMILY:
            part, pnote = FAMILY[val]
            note = pnote or note
        elif all(r in PARTS for r in refs) and len({PARTS[r][0] for r in refs}) == 1:
            part = PARTS[refs[0]][0]
            # keep every ref's note, not just the first — three MCP6002s do
            # three different jobs and the BOM should say so
            seen, notes = set(), []
            for r in refs:
                n = PARTS[r][1]
                if n and n not in seen:
                    seen.add(n)
                    notes.append(f"{r}: {n}" if len(refs) > 1 else n)
            note = "; ".join(notes) or note
        if g["annot"]:
            annot = "; ".join(sorted(g["annot"]))
            note = f"{note} ({annot})" if note else annot
        sheets = sorted(g["sheets"])
        rows.append({
            "Qty": len(refs), "Value": val, "Package": pkg,
            "Part / spec": part, "Refs": " ".join(refs),
            "Subsystem": "shared: " + ", ".join(sheets) if len(sheets) > 1
                         else sheets[0], "Note": note,
        })

    def bucket(r):
        """Multi-sheet lines get their own section rather than being filed
        under whichever sheet happens to sort first."""
        return "Shared across sheets" if r["Subsystem"].startswith("shared:") \
            else r["Subsystem"]

    rows.sort(key=lambda r: (SHEET_ORDER.index(bucket(r))
                             if bucket(r) in SHEET_ORDER else 99,
                             sortkey(r["Refs"].split()[0])))

    csv_path = os.path.join(OUT, "ecu25-main-bom.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    md = ["# ECU-25 main board — bill of materials", "",
          f"{len(comps)} placed components in {len(rows)} order lines. Generated by",
          "`hardware/kicad/gen/gen_bom.py` from the schematic netlist — do not hand-edit;",
          "component values are set in `gen_ecu25.py` and justified in",
          "[docs/circuits/README.md](../circuits/README.md).", "",
          "For breadboard prototyping (through-hole substitutes, module-based build,",
          "safety notes) see [breadboard-prototype.md](breadboard-prototype.md).", ""]
    by_sheet = collections.OrderedDict((s, []) for s in SHEET_ORDER)
    for r in rows:
        by_sheet.setdefault(bucket(r), []).append(r)
    for sheet, srows in by_sheet.items():
        if not srows:
            continue
        md += [f"## {sheet}  ({sum(r['Qty'] for r in srows)} parts)", ""]
        if sheet == "Shared across sheets":
            md += ["Lines whose parts appear on more than one schematic sheet "
                   "(decoupling, pull-ups, common series resistors).", ""]
        md += ["| Qty | Value | Package | Part / spec | Refs | Used on | Note |",
               "|----:|-------|---------|-------------|------|---------|------|"]
        for r in srows:
            used = r["Subsystem"].replace("shared: ", "")
            md.append(f"| {r['Qty']} | {r['Value']} | {r['Package']} | "
                      f"{r['Part / spec']} | {r['Refs']} | {used} | {r['Note']} |")
        md.append("")
    md_path = os.path.join(OUT, "ecu25-main-bom.md")
    open(md_path, "w").write("\n".join(md))

    print(f"{len(comps)} components -> {len(rows)} order lines")
    print("wrote", csv_path)
    print("wrote", md_path)


if __name__ == "__main__":
    main()
