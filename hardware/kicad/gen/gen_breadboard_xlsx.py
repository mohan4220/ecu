"""
gen_breadboard_xlsx.py — turns docs/bom/breadboard-prototype.md into a
formatted workbook for bench use.

The markdown is the reviewed source of truth; this script parses it rather
than restating it, so the two cannot drift apart. Anything it cannot place
is a hard error, never a silent drop — an incomplete workbook on a bench is
worse than no workbook.

Output: docs/bom/ecu25-breadboard-bom.xlsx

  Cover           what this is, and which revision it came from
  Safety          the warnings, in full
  Shopping List   every part summed across blocks, with a Buy column
  Shared Kit      tools, dev board and supplies bought once
  A .. H          one sheet per block: parts, wiring nodes, warnings, test
  Build Order     the recommended sequence

Usage:
  .venv/bin/python hardware/kicad/gen/gen_breadboard_xlsx.py
"""

import os, re, sys, subprocess, collections

try:
    from openpyxl import Workbook
    from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
    from openpyxl.utils import get_column_letter
    from openpyxl.worksheet.properties import PageSetupProperties
except ImportError:
    sys.exit("openpyxl not installed:  .venv/bin/pip install openpyxl")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DOCS = os.path.join(ROOT, "docs", "bom")
SRC = os.path.join(DOCS, "breadboard-prototype.md")
OUT = os.path.join(DOCS, "ecu25-breadboard-bom.xlsx")

# ---------------------------------------------------------------- styling

NAVY = "1F3A5F"
HEAD = PatternFill("solid", fgColor=NAVY)
TINT = PatternFill("solid", fgColor="DCE6F1")
ALT = PatternFill("solid", fgColor="F2F6FA")
WARN = PatternFill("solid", fgColor="F8D7DA")
KIT = PatternFill("solid", fgColor="E8F4EA")
WHITE_BOLD = Font(color="FFFFFF", bold=True, size=11)
TITLE = Font(bold=True, size=14, color=NAVY)
SUB = Font(italic=True, size=10, color="555555")
BOLD = Font(bold=True, size=10)
DANGER = Font(bold=True, size=10, color="9C1C24")
BODY = Font(size=10)
MONO = Font(name="Consolas", size=9)
THIN = Side(style="thin", color="B0B8C4")
BOX = Border(left=THIN, right=THIN, top=THIN, bottom=THIN)
WRAP = Alignment(wrap_text=True, vertical="top")

# prose that must not first be read after the parts table
IMPERATIVE = re.compile(
    r"never|not optional|read the safety|must be soldered|do not\b|"
    r"and it matters|measure before", re.I)


def md_to_text(s):
    """Flatten inline markdown to something readable in a cell."""
    s = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", s)        # links -> label
    s = s.replace("**", "").replace("`", "")
    s = re.sub(r"(?<!\w)\*([^*]+)\*(?!\w)", r"\1", s)     # italics
    return s.strip()


def md_links(s):
    return re.findall(r"\[([^\]]+)\]\(([^)]+)\)", s)


# ---------------------------------------------------------------- parsing

PARTS_HEADER = {"Qty", "Prototype part"}


def parse_table(lines, i):
    rows = []
    while i < len(lines) and lines[i].lstrip().startswith("|"):
        cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
        if not all(re.fullmatch(r":?-+:?", c) for c in cells):
            rows.append([md_to_text(c) for c in cells])
        i += 1
    return rows, i


def read_para(lines, i):
    """Collect a paragraph. Markdown list items are returned as separate
    entries so they do not run together into one sentence."""
    items, buf = [], []

    def flush():
        if buf:
            items.append(" ".join(buf))
            buf.clear()

    while i < len(lines) and lines[i].strip() \
            and not lines[i].lstrip().startswith("|"):
        ln = lines[i].strip()
        if re.match(r"^[-*+]\s|^\d+\.\s", ln):
            flush()
            buf.append(re.sub(r"^([-*+]|\d+\.)\s+", "• ", ln))
        elif buf and buf[-1].endswith("-") and ln[:1].islower():
            buf[-1] = buf[-1][:-1] + ln       # rejoin a hyphen-broken word
        else:
            buf.append(ln)
        i += 1
    flush()
    return [md_to_text(x) for x in items], i


def parse(path):
    raw = open(path, encoding="utf-8").read().split("\n")
    doc = {"intro": [], "kit": [], "blocks": [], "safety": [], "order": [],
           "order_note": [], "convention": []}
    dropped = []
    i, section = 0, "intro"

    while i < len(raw):
        ln = raw[i]

        m = re.match(r"^## Block (\w+)\s*[—–-]\s*(.+)$", ln)
        if m:
            doc["blocks"].append({
                "id": m.group(1), "title": m.group(2), "ref": "",
                "reflinks": [], "nodes": "", "table": [], "test": [],
                "pre": [], "post": []})
            section = "block"
            i += 1
            continue

        if ln.startswith("## "):
            h = ln[3:].strip()
            section = ("kit" if h.startswith("Shared kit") else
                       "safety" if h.startswith("Safety") else
                       "order" if h.startswith("Suggested build order") else
                       "skip")
            i += 1
            continue

        if ln.startswith("### "):
            head = md_to_text(ln[4:])
            if section == "safety":
                doc["safety"].append(("H", head))
            elif section in ("kit", "convention"):
                section = "convention"
                doc["convention"].append(("H", head))
            i += 1
            continue

        if ln.lstrip().startswith("|"):
            rows, i = parse_table(raw, i)
            if section == "kit":
                doc["kit"] = rows
            elif section == "block" and doc["blocks"]:
                b = doc["blocks"][-1]
                if PARTS_HEADER <= set(rows[0]) and not b["table"]:
                    b["table"] = rows
                else:
                    (b["post"] if b["table"] else b["pre"]).append(
                        ("TABLE", rows))
            else:
                dropped.append((section, rows[0][:3]))
            continue

        if not ln.strip() or ln.startswith("---"):
            i += 1
            continue

        if section == "block" and doc["blocks"]:
            b = doc["blocks"][-1]
            if ln.startswith(("Reference:", "References:")):
                para, i = read_para(raw, i)
                b["reflinks"] = md_links(" ".join(raw[i - len(para):i]))
                b["ref"] = " ".join(para).split(":", 1)[1].strip()
                continue
            is_nodes = ln.startswith("**Nodes:**")
            is_test = ln.startswith("**Test:**")
            para, i = read_para(raw, i)
            if is_nodes:
                b["nodes"] = re.sub(r"^Nodes:\s*", "", " ".join(para))
            elif is_test:
                b["test"] = [re.sub(r"^Test:\s*", "", para[0])] + para[1:]
                # a Test may continue over further paragraphs
                while i < len(raw):
                    j = i
                    while j < len(raw) and not raw[j].strip():
                        j += 1
                    if j >= len(raw) or raw[j].lstrip()[:2] in ("--", "##") \
                            or raw[j].lstrip().startswith("|"):
                        break
                    more, i = read_para(raw, j)
                    b["test"] += more
            else:
                (b["post"] if b["table"] else b["pre"]).extend(
                    ("P", p) for p in para)
            continue

        if section in ("safety", "convention"):
            para, i = read_para(raw, i)
            doc["safety" if section == "safety" else "convention"].extend(
                ("P", p) for p in para)
            continue

        if section == "order":
            if re.match(r"^\d+\. ", ln):
                doc["order"].append(md_to_text(ln))
                i += 1
            else:
                para, i = read_para(raw, i)
                doc["order_note"].extend(para)
            continue

        if section == "intro":
            para, i = read_para(raw, i)
            doc["intro"].extend(para)
            continue

        i += 1

    if dropped:
        sys.exit(f"parser could not place {len(dropped)} table(s): {dropped}\n"
                 "fix the parser rather than shipping a partial workbook")
    if not doc["blocks"]:
        sys.exit("no '## Block X — ...' sections found")
    empty = [b["id"] for b in doc["blocks"] if not b["table"]]
    if empty:
        sys.exit(f"blocks with no parts table (its header must contain "
                 f"{sorted(PARTS_HEADER)}): {empty}")
    return doc


# ------------------------------------------------------ shopping list roll-up

CATEGORY = [
    (r"\bLED\b", "Diode / LED"),
    (r"\bMΩ|\bkΩ|\bΩ\b|\bpot\b|decade|wirewound", "Resistor"),
    (r"\bnF\b|\bµF\b|\buF\b|\bpF\b|film|ceramic|electrolytic", "Capacitor"),
    (r"1N4148|1N5819|zener|TVS|diode", "Diode / LED"),
    (r"BC337|BC557|2N7000|transistor", "Transistor"),
    (r"LM358|LM393|MCP6002|25LC|module|MAX3485|SN65", "IC / module"),
    (r"relay|G5LE", "Relay"),
    (r"transformer|CT\b|split-core|generator|supply|adapter|ST-Link|board",
     "Equipment"),
    (r"switch|jumper", "Hardware"),
]
CAT_ORDER = ["Resistor", "Capacitor", "Diode / LED", "Transistor",
             "IC / module", "Relay", "Hardware", "Equipment", "Other"]

# blocks the markdown marks optional — buying their parts is a choice
OPTIONAL_BLOCKS = {"H"}

# already supplied in bulk on the Shared Kit sheet
KIT_COVERED = {"100 nF", "10 µF"}

SPECIALTY = re.compile(r"wirewound|pot\b|decade|TVS|zener|film, 100 V"
                       r"|(?<![\d/])[23] W\b", re.I)


def categorise(part):
    for pat, cat in CATEGORY:
        if re.search(pat, part, re.I):
            return cat
    return "Other"


def spares(qty, cat, part=""):
    """Ordinary small passives come in strips, so round up to a usable
    minimum. Specialty parts get one spare. The relay gets one too: its pins
    are soldered by hand and its pin map has already been got wrong once."""
    if SPECIALTY.search(part):
        return qty + 1
    if cat in ("Resistor", "Capacitor", "Diode / LED", "Transistor"):
        return max(5, qty + 3)
    if cat in ("IC / module", "Relay"):
        return qty + 1
    return qty


def natkey(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


def rollup(doc):
    agg, total = collections.OrderedDict(), 0
    for b in doc["blocks"]:
        idx = {name: n for n, name in enumerate(b["table"][0])}
        qi, pi, ri = idx["Qty"], idx["Prototype part"], idx.get("Production")
        for row in b["table"][1:]:
            if len(row) <= pi:
                continue
            part = re.sub(r"\s+", " ", row[pi]).strip()
            if not part or part.startswith(("—", "Bias from")):
                continue
            n = int(row[qi]) if str(row[qi]).strip().isdigit() else 0
            total += n
            key = re.sub(r"\s*\((optional|TO-92|DIP-8|DO-41)\)", "", part).strip()
            e = agg.setdefault(key, {"qty": 0, "blocks": [], "prod": set()})
            e["qty"] += n
            e["blocks"].append((b["id"], n))
            if ri is not None and len(row) > ri and row[ri] and row[ri] != "—":
                e["prod"].add(row[ri])
    return agg, total


# ---------------------------------------------------------------- writing

def setup_print(ws, title_rows=None):
    ws.page_setup.orientation = "landscape"
    ws.sheet_properties.pageSetUpPr = PageSetupProperties(fitToPage=True)
    ws.page_setup.fitToWidth = 1
    ws.page_setup.fitToHeight = 0
    if title_rows:
        ws.print_title_rows = title_rows


def style_header(ws, headers, widths=None, row=1):
    for c, h in enumerate(headers, 1):
        cell = ws.cell(row=row, column=c, value=h)
        cell.fill, cell.font, cell.border = HEAD, WHITE_BOLD, BOX
        cell.alignment = Alignment(vertical="center", wrap_text=True)
        if widths:
            ws.column_dimensions[get_column_letter(c)].width = widths[c - 1]
    ws.row_dimensions[row].height = 22


def put_row(ws, r, values, fill=None, wrap_cols=()):
    for c, v in enumerate(values, 1):
        cell = ws.cell(row=r, column=c, value=v)
        cell.font, cell.border = BODY, BOX
        cell.alignment = WRAP if c in wrap_cols else Alignment(vertical="top")
        if fill:
            cell.fill = fill
    return r + 1


def para(ws, r, text, ncols, font=BODY, fill=None, width=110):
    cell = ws.cell(row=r, column=1, value=text)
    cell.font, cell.alignment = font, WRAP
    if fill:
        cell.fill = fill
    if ncols > 1:
        ws.merge_cells(start_row=r, start_column=1, end_row=r, end_column=ncols)
    ws.row_dimensions[r].height = max(15, 13 * (len(text) // width + 1))
    return r + 1


def git_rev():
    try:
        rev = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain",
                                "docs/bom/breadboard-prototype.md"],
                               capture_output=True, text=True, timeout=10)
        return (rev.stdout.strip() or "unknown") + \
               (" + uncommitted edits" if dirty.stdout.strip() else "")
    except Exception:
        return "unknown"


def sheet_cover(wb, doc):
    ws = wb.create_sheet("Cover")
    ws.column_dimensions["A"].width = 112
    ws["A1"] = "ECU-25 — breadboard prototype"
    ws["A1"].font = Font(bold=True, size=18, color=NAVY)
    r = 3
    for p in doc["intro"]:
        r = para(ws, r, p, 1) + 1
    r += 1
    r = para(ws, r, "Sheets in this workbook", 1, font=BOLD)
    for line in [
            "Safety — read before building blocks F and G.",
            "Shopping List — every part across blocks A-H, summed, with spares.",
            "Shared Kit — tools, dev board and supplies bought once.",
            "A .. H — one sheet per block: wiring nodes, parts, warnings, test.",
            "Build Order — the recommended sequence."]:
        r = para(ws, r, "• " + line, 1)
    r += 1
    para(ws, r, f"Generated from docs/bom/breadboard-prototype.md at revision "
                f"{git_rev()} by hardware/kicad/gen/gen_breadboard_xlsx.py — "
                f"edit the markdown, not this file.", 1, font=SUB)
    setup_print(ws)
    return ws


def sheet_shopping(wb, doc):
    ws = wb.create_sheet("Shopping List")
    ws["A1"] = "Consolidated shopping list — blocks A to H"
    ws["A1"].font = TITLE
    ws["A2"] = ("Every part used across the blocks, summed. 'Buy' adds spares: "
                "ordinary passives to a usable minimum, one spare per IC and "
                "for the relay. Tick 'Got' as you buy. Tools and the dev board "
                "are on the Shared Kit sheet — note its resistor and capacitor "
                "assortments do NOT cover the 1 % values or the film capacitors "
                "listed here, so buy those separately.")
    ws["A2"].font = SUB
    ws["A2"].alignment = WRAP
    ws.merge_cells("A1:G1")
    ws.merge_cells("A2:G2")
    ws.row_dimensions[2].height = 42

    headers = ["Category", "Part", "Used", "Buy", "Got", "Blocks (qty)",
               "Replaces (production ref)"]
    style_header(ws, headers, [14, 40, 7, 7, 6, 18, 30], row=4)
    ws.freeze_panes = "A5"
    ws.auto_filter.ref = "A4:G4"
    setup_print(ws, title_rows="4:4")

    agg, _ = rollup(doc)
    by_cat = collections.defaultdict(list)
    for part, e in agg.items():
        by_cat[categorise(part)].append((part, e))

    r = 5
    for cat in CAT_ORDER:
        items = by_cat.get(cat)
        if not items:
            continue
        for c in range(1, len(headers) + 1):
            cell = ws.cell(row=r, column=c, value=cat if c == 1 else None)
            cell.fill, cell.border = TINT, BOX
            cell.font = Font(bold=True, size=10, color=NAVY)
        r += 1
        for part, e in sorted(items, key=lambda x: natkey(x[0])):
            per_block = collections.Counter()
            for bid, n in e["blocks"]:
                per_block[bid] += n
            blocks = ", ".join(f"{bid}×{n}" if n > 1 else bid
                               for bid, n in sorted(per_block.items()))
            if set(per_block) <= OPTIONAL_BLOCKS:
                blocks += "  (optional block)"
            prod = ", ".join(sorted(e["prod"], key=natkey))
            if part in KIT_COVERED:
                prod = (prod + " — " if prod else "") + "also in the shared kit"
            r = put_row(ws, r, ["", part, e["qty"] or "",
                                spares(e["qty"], cat, part), "", blocks, prod],
                        fill=ALT if r % 2 else None, wrap_cols=(2, 7))
        r += 1
    return ws


def sheet_kit(wb, doc):
    ws = wb.create_sheet("Shared Kit")
    ws["A1"] = "Shared kit — buy once, used by every block"
    ws["A1"].font = TITLE
    ws.merge_cells("A1:C1")
    style_header(ws, doc["kit"][0], [7, 42, 72], row=3)
    ws.freeze_panes = "A4"
    setup_print(ws, title_rows="3:3")
    r = 4
    for row in doc["kit"][1:]:
        warn = row[0].strip() == "—" and row[1].startswith("Not")
        r = put_row(ws, r, row, fill=WARN if warn else (ALT if r % 2 else None),
                    wrap_cols=(2, 3))
    r += 1
    for kind, text in doc["convention"]:
        r = para(ws, r, text, 3, font=TITLE if kind == "H" else BODY,
                 fill=None if kind == "H" else KIT)
    return ws


def short_title(t):
    t = re.split(r"[,(—]", t)[0].strip()
    t = re.sub(r"[\\/*?:\[\]]", " ", t)
    return re.sub(r"\s+", " ", t).strip()[:22]


def emit_prose(ws, r, items, ncols):
    for kind, payload in items:
        if kind == "TABLE":
            for n, row in enumerate(payload):
                if n == 0:
                    # no widths: a secondary table must not resize the sheet
                    style_header(ws, row, row=r)
                else:
                    put_row(ws, r, row, wrap_cols=(2,))
                r += 1
        else:
            warn = bool(IMPERATIVE.search(payload))
            r = para(ws, r, payload, ncols, font=DANGER if warn else BODY,
                     fill=WARN if warn else None)
        r += 1
    return r


def sheet_block(wb, b):
    ws = wb.create_sheet(f"{b['id']} - {short_title(b['title'])}")
    NC = 4
    ws["A1"] = f"Block {b['id']} — {b['title']}"
    ws["A1"].font = TITLE
    ws.merge_cells("A1:D1")
    refs = ", ".join(f"docs/circuits/{os.path.basename(h)}"
                     for _, h in b["reflinks"]) or b["ref"]
    ws["A2"] = f"Reference schematic: {refs}"
    ws["A2"].font = SUB
    ws.merge_cells("A2:D2")
    setup_print(ws)

    r = 4
    # whatever the markdown put BEFORE the parts table stays before it —
    # those are the read-this-first paragraphs
    if b["pre"]:
        r = emit_prose(ws, r, b["pre"], NC)

    if b["nodes"]:
        r = para(ws, r, "Wiring (nodes)", NC, font=BOLD)
        r = para(ws, r, b["nodes"], NC, font=MONO, fill=TINT, width=100) + 1

    style_header(ws, b["table"][0], [7, 34, 16, 78], row=r)
    ws.freeze_panes = ws.cell(row=r + 1, column=1)
    ws.print_title_rows = f"{r}:{r}"
    r += 1
    for row in b["table"][1:]:
        r = put_row(ws, r, row, fill=ALT if r % 2 else None, wrap_cols=(2, 4))

    if b["post"]:
        r = emit_prose(ws, r + 1, b["post"], NC)

    if b["test"]:
        r = para(ws, r + 1, "Test", NC, font=BOLD)
        for p in b["test"]:
            r = para(ws, r, p, NC, fill=KIT)
    return ws


def sheet_order(wb, doc):
    ws = wb.create_sheet("Build Order")
    ws["A1"] = "Suggested build order"
    ws["A1"].font = TITLE
    ws.column_dimensions["A"].width = 110
    setup_print(ws)
    r = 3
    for line in doc["order"]:
        cell = ws.cell(row=r, column=1, value=line)
        cell.font, cell.alignment, cell.border = BODY, WRAP, BOX
        r += 1
    r += 1
    for p in doc["order_note"]:
        r = para(ws, r, p, 1)
    return ws


def sheet_safety(wb, doc):
    ws = wb.create_sheet("Safety")
    ws["A1"] = "SAFETY — read before building blocks F and G"
    ws["A1"].font = Font(bold=True, size=16, color="9C1C24")
    ws.column_dimensions["A"].width = 112
    setup_print(ws)
    r = 3
    for kind, text in doc["safety"]:
        if kind == "H":
            r = para(ws, r, text, 1,
                     font=Font(bold=True, size=12, color="9C1C24"))
        else:
            # the pink is for the imperatives — if everything is highlighted,
            # nothing is
            warn = bool(IMPERATIVE.search(text))
            r = para(ws, r, text, 1, font=DANGER if warn else BODY,
                     fill=WARN if warn else None, width=105) + 1
    return ws


def main():
    doc = parse(SRC)
    agg, total = rollup(doc)
    listed = sum(e["qty"] for e in agg.values())
    if listed != total:
        sys.exit(f"roll-up lost units: block tables total {total}, "
                 f"shopping list totals {listed}")

    wb = Workbook()
    wb.remove(wb.active)
    sheet_cover(wb, doc)
    sheet_safety(wb, doc)
    sheet_shopping(wb, doc)
    sheet_kit(wb, doc)
    for b in doc["blocks"]:
        sheet_block(wb, b)
    sheet_order(wb, doc)
    wb.save(OUT)

    print(f"{len(doc['blocks'])} blocks, {len(doc['kit']) - 1} kit items, "
          f"{len(agg)} distinct parts, {total} units")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
