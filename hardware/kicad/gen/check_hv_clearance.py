"""
check_hv_clearance.py — verify high-voltage spacing on the routed board.

kicad-cli 7 has no DRC subcommand, so this board's design-rule check is a GUI
step that is easy to skip. The 415 V spacing is the one rule here that hurts
somebody if it is wrong, so it gets a headless check that can run in a script
and fail loudly.

This does NOT invent its own rules. It reads the netclass clearances out of
the board and the custom rules out of ecu25-main.kicad_dru, then checks the
copper geometry against them. A checker carrying its own copy of the rules is
worse than no checker: it drifts, and then it either passes a real violation
or fails a legal board until somebody edits it to shut it up.

What it checks: every copper item on a high-voltage net stands at least the
required distance from every copper item of a different net on the same layer.

What it does NOT check: creepage across a milled slot, clearance to the board
edge, hole-to-hole spacing, or anything else DRC does. It is one rule checked
properly, not a replacement for DRC in pcbnew.

Usage:
  python3 check_hv_clearance.py <board.kicad_pcb> [rules.kicad_dru]
"""

import os
import re
import sys

import pcbnew

# Netclasses whose spacing this tool is responsible for.
HV_CLASSES = ("HV_AC", "HV_CHAIN")



# ----------------------------------------------------------------- rule file

class Atom:
    """One `A.NetName == 'x'` / `A.NetClass != 'y'` comparison."""

    def __init__(self, side, prop, op, want):
        self.side, self.prop, self.op, self.want = side, prop, op, want

    def test(self, a, b):
        item = a if self.side == "A" else b
        got = item.get(self.prop)
        return got != self.want if self.op == "!=" else got == self.want


class Op:
    def __init__(self, kind, kids):
        self.kind, self.kids = kind, kids

    def test(self, a, b):
        if self.kind == "&&":
            return all(k.test(a, b) for k in self.kids)
        return any(k.test(a, b) for k in self.kids)


ATOM_RE = re.compile(
    r"^\s*([AB])\.(NetName|NetClass)\s*(==|!=)\s*'([^']*)'\s*$")


def parse_condition(text):
    """
    A deliberately small parser for the subset of KiCad's rule language this
    project uses: A/B property comparisons joined by && and ||, && binding
    tighter. Anything else raises, because a condition this tool cannot read
    is a rule it would otherwise ignore, and a rule silently ignored by a
    safety check is the whole problem.
    """
    ors = split_top(text, "||")
    branches = []
    for clause in ors:
        ands = split_top(clause, "&&")
        atoms = []
        for term in ands:
            term = term.strip()
            while term.startswith("(") and term.endswith(")") and \
                    balanced(term[1:-1]):
                term = term[1:-1].strip()
            m = ATOM_RE.match(term)
            if not m:
                raise SystemExit(
                    f"cannot read rule condition {term!r}. This tool only "
                    f"understands A/B NetName and NetClass == / != "
                    f"comparisons joined by && and ||. Teach it the new form "
                    f"rather than letting "
                    f"the rule go unchecked.")
            atoms.append(Atom(m.group(1), m.group(2),
                              m.group(3), m.group(4)))
        branches.append(atoms[0] if len(atoms) == 1 else Op("&&", atoms))
    return branches[0] if len(branches) == 1 else Op("||", branches)


def balanced(s):
    depth = 0
    for ch in s:
        depth += (ch == "(") - (ch == ")")
        if depth < 0:
            return False
    return depth == 0


def split_top(text, sep):
    """Split on `sep` at paren depth 0 and outside quotes."""
    out, depth, quote, start, i = [], 0, False, 0, 0
    while i < len(text):
        ch = text[i]
        if ch == "'":
            quote = not quote
        elif not quote:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif depth == 0 and text.startswith(sep, i):
                out.append(text[start:i])
                i += len(sep)
                start = i
                continue
        i += 1
    out.append(text[start:])
    return out


def sexp(text):
    toks = re.findall(r"\(|\)|\"[^\"]*\"|[^\s()\"]+", text)
    def walk(pos):
        out = []
        while pos < len(toks):
            t = toks[pos]
            if t == "(":
                sub, pos = walk(pos + 1)
                out.append(sub)
            elif t == ")":
                return out, pos + 1
            else:
                out.append(t.strip('"'))
                pos += 1
        return out, pos
    return walk(0)[0]


def load_rules(path):
    """[(name, condition, clearance_nm)] in file order."""
    text = re.sub(r"(?m)^\s*#.*$", "", open(path, encoding="utf-8").read())
    rules = []
    for node in sexp(text):
        if not (isinstance(node, list) and node and node[0] == "rule"):
            continue
        name = node[1]
        cond = clear = None
        for sub in node[2:]:
            if not isinstance(sub, list) or not sub:
                continue
            if sub[0] == "condition":
                cond = parse_condition(sub[1])
            elif sub[0] == "constraint" and sub[1] == "clearance":
                for c in sub[2:]:
                    if isinstance(c, list) and c and c[0] == "min":
                        clear = int(round(float(
                            c[1].rstrip("m")) * 1e6))
        # A rule with no (condition ...) matches EVERYTHING in KiCad, and
        # being last in the file it would override every rule above it.
        # Dropping such a rule quietly is the exact failure this tool is
        # supposed to not have.
        if clear is None:
            continue  # not a clearance rule; some other constraint type
        if cond is None:
            raise SystemExit(
                f"rule {name!r} sets a clearance with no condition, so it "
                f"applies to everything. This tool will not guess at that — "
                f"give it a condition or teach this parser the case.")
        rules.append((name, cond, clear))
    return rules


# ------------------------------------------------------------------ geometry

def copper_layers(board):
    """
    Copper layers this board actually has.

    A through-hole pad's LayerSet spans every copper layer KiCad can define,
    not the ones in the stackup, so walking CuStack() on a 2-layer board
    reports the same pad on thirty layers that do not exist.
    """
    return {l for l in board.GetEnabledLayers().CuStack()
            if pcbnew.IsCopperLayer(l)}


def item_shape(item, layer):
    """
    Copper outline of one item on one layer.

    GetEffectiveShape() rather than TransformShapeToPolygon(): the polygon
    call's 4-argument overload exists ONLY for PAD. On a PCB_TRACK or PCB_VIA
    it demands an ERROR_LOC argument, and KiCad 7's bindings do not export
    that enum at all — neither ERROR_INSIDE nor an integer is accepted. So
    the polygon route works on an unrouted board and raises TypeError on the
    first track, which is to say it fails on exactly the board this tool
    exists to check. GetEffectiveShape has no such hole and gives the same
    Collide semantics.
    """
    return item.GetEffectiveShape(layer)


def copper_items(board, layers):
    """
    (item, ref, netcode, layer, shape, bbox) for everything carrying copper.

    Zones are in here deliberately. The two GND pours are about 19,000 and
    21,000 mm2 of copper, far and away the largest conductors on the board,
    and leaving them out meant a pour creeping to within 0.4 mm of a 415 V
    net would still have been reported as a PASS.
    """
    built = []

    def add(item, ref, code, layer, shape, bbox):
        if shape is not None:
            built.append((item, ref, code, layer, shape, bbox))

    for fp in board.GetFootprints():
        ref = fp.GetReference()
        for pad in fp.Pads():
            for layer in pad.GetLayerSet().CuStack():
                if layer in layers:
                    add(pad, ref, pad.GetNetCode(), layer,
                        item_shape(pad, layer), pad.GetBoundingBox())
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            for layer in t.GetLayerSet().CuStack():
                if layer in layers:
                    add(t, None, t.GetNetCode(), layer,
                        item_shape(t, layer), t.GetBoundingBox())
        else:
            add(t, None, t.GetNetCode(), t.GetLayer(),
                item_shape(t, t.GetLayer()), t.GetBoundingBox())
    for z in board.Zones():
        if z.GetIsRuleArea():
            continue
        for layer in z.GetLayerSet().CuStack():
            if layer not in layers:
                continue
            filled = z.GetFilledPolysList(layer)
            if filled is None or filled.OutlineCount() == 0:
                continue
            add(z, None, z.GetNetCode(), layer, filled, z.GetBoundingBox())
    return built


def describe(item, ref=None):
    """
    GetParentFootprint() comes back as a BOARD_ITEM_CONTAINER in KiCad 7's
    bindings with no GetReference() on it, so the reference is captured while
    walking the footprints instead.
    """
    if isinstance(item, pcbnew.PAD):
        return f"pad {ref or '?'}.{item.GetNumber()}"
    if isinstance(item, pcbnew.ZONE):
        return "zone fill"
    is_via = isinstance(item, pcbnew.PCB_VIA)
    p = item.GetPosition() if is_via else item.GetStart()
    return f"{'via' if is_via else 'track'} at ({p.x/1e6:.2f},{p.y/1e6:.2f})"


# --------------------------------------------------------------------- check

def required(rules, netclass_clear, floor, a, b):
    """
    Clearance demanded between two items, in nm.

    Starts from the netclass floor (KiCad uses the larger of the two classes)
    and then lets custom rules override it. Later rules win: this project's
    own rule file relies on that, putting ac_own_chain_* and netless_pads
    after the blanket ac_line_clearance precisely so they can relax it.
    """
    need = max(netclass_clear.get(a["NetClass"], 0),
               netclass_clear.get(b["NetClass"], 0))
    hit = None
    for name, cond, clear in rules:
        if cond.test(a, b) or cond.test(b, a):
            hit = (name, clear)
    if hit:
        need, why = hit[1], hit[0]
    else:
        why = "netclass"
    # KiCad clamps the resolved clearance UP to the board minimum after every
    # rule has had its say (drc_engine.cpp). Without this a custom rule that
    # relaxes below the board minimum would make this tool LOOSER than the
    # DRC it stands in for, which is the direction that gets someone hurt.
    if floor > need:
        return floor, f"{why}, raised to board minimum"
    return need, why


def check(board_path, dru_path):
    board = pcbnew.LoadBoard(board_path)
    rules = load_rules(dru_path)
    print(f"{board.GetCopperLayerCount()}-layer board; "
          f"{len(rules)} custom rules from {os.path.basename(dru_path)}")

    netclass_clear, classes, names = {}, {}, {}
    for n in [str(x) for x in board.GetNetsByName().keys()]:
        net = board.FindNet(n)
        if net is None:
            continue
        code = net.GetNetCode()
        classes[code] = net.GetNetClassName()
        names[code] = net.GetNetname()
        nc = board.GetAllNetClasses().get(net.GetNetClassName())
        if nc is not None:
            netclass_clear[net.GetNetClassName()] = nc.GetClearance()

    floor = board.GetDesignSettings().m_MinClearance
    print(f"board minimum clearance {floor/1e6:.3f} mm")

    layers = copper_layers(board)
    items = copper_items(board, layers)
    hv = [t for t in items if classes.get(t[2]) in HV_CLASSES]
    if not hv:
        raise SystemExit("no nets in an HV class — is this the right board?")
    print(f"{len(items)} copper items, {len(hv)} on {'/'.join(HV_CLASSES)}")

    def facts(code):
        return {"NetName": names.get(code, ""),
                "NetClass": classes.get(code, "Default")}

    found = {}
    for item, ref, code, layer, poly, bbox in hv:
        fa = facts(code)
        for oitem, oref, ocode, olayer, opoly, obbox in items:
            if ocode == code or olayer != layer or oitem is item:
                continue
            need, why = required(rules, netclass_clear, floor, fa, facts(ocode))
            probe = pcbnew.BOX2I(
                pcbnew.VECTOR2I(bbox.GetX(), bbox.GetY()),
                pcbnew.VECTOR2I(bbox.GetWidth(), bbox.GetHeight()))
            probe.Inflate(need)
            if not probe.Intersects(obbox):
                continue
            if poly.Collide(opoly, need):
                a, b = describe(item, ref), describe(oitem, oref)
                key = tuple(sorted([a, b])) + (board.GetLayerName(layer),)
                found[key] = (board.GetLayerName(layer), a, names.get(code, ""),
                              b, names.get(ocode, ""), need / 1e6, why)

    if not found:
        print("PASS: HV copper meets every clearance the project asks for")
        return 0
    print(f"FAIL: {len(found)} clearance violations")
    for lname, a, na, b, nb, need, why in sorted(found.values()):
        print(f"  {lname}: {a} ({na}) too close to {b} ({nb}) "
              f"— needs {need} mm [{why}]")
    return 1


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    pcb = sys.argv[1]
    dru = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.splitext(pcb)[0] + ".kicad_dru"
    if not os.path.exists(dru):
        raise SystemExit(f"no rule file at {dru}")
    sys.exit(check(pcb, dru))


if __name__ == "__main__":
    main()
