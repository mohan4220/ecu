"""
kisch.py — programmatic KiCad 7 schematic builder for ECU-25.

Wraps kiutils with a placement API: load symbols from the system libraries,
flatten `extends` chains, embed them in the sheet's lib_symbols, place
instances on grid, and compute absolute pin coordinates so wires and labels
land exactly on pin ends.

Coordinate convention: schematic space, mm, +Y down (KiCad native).
Symbol libraries store geometry +Y up; pin transforms handle the flip.
"""

import math
import uuid as uuidlib
from copy import deepcopy

from kiutils.schematic import Schematic
from kiutils.symbol import SymbolLib
from kiutils.items.schitems import (
    SchematicSymbol, LocalLabel, GlobalLabel, HierarchicalLabel, Junction,
    Connection, NoConnect, HierarchicalSheet, HierarchicalSheetInstance,
    HierarchicalPin, Text, SymbolProjectInstance, SymbolProjectPath,
    HierarchicalSheetProjectInstance, HierarchicalSheetProjectPath,
)
from kiutils.items.common import (Property, Position, Effects, Font, Stroke,
                                  ColorRGBA, Justify)
from kiutils.items.schitems import Fill

SYMDIR = "/usr/share/kicad/symbols"
GRID = 1.27

def snap(v):
    return round(round(v / GRID) * GRID, 4)

def font(h=1.27, hide=False, justify_l=False):
    e = Effects(font=Font(height=h, width=h), hide=hide)
    if justify_l:
        e.justify = Justify(horizontally="left")
    return e


class LibCache:
    """Loads .kicad_sym libraries once, resolves derived (extends) symbols."""

    def __init__(self):
        self._libs = {}

    def _lib(self, nickname):
        if nickname not in self._libs:
            lib = SymbolLib.from_file(f"{SYMDIR}/{nickname}.kicad_sym")
            self._libs[nickname] = {s.entryName: s for s in lib.symbols}
        return self._libs[nickname]

    def resolve(self, lib_id):
        """Return a flattened copy of the symbol (extends chain merged)."""
        nickname, name = lib_id.split(":")
        lib = self._lib(nickname)
        sym = deepcopy(lib[name])
        while sym.extends:
            parent = deepcopy(lib[sym.extends])
            # child keeps its own properties; graphics/pins/units come from parent
            merged_props = {p.key: p for p in parent.properties}
            for p in sym.properties:
                merged_props[p.key] = p
            parent.properties = list(merged_props.values())
            parent.entryName = sym.entryName
            parent.extends = sym.extends = None
            # units inside parent are named <parentName>_<unit>_<style>;
            # rename them to match the child or KiCad rejects the file
            for u in parent.units:
                u.entryName = sym.entryName
            sym = parent
        sym.libraryNickname = nickname
        return sym


def _sym_pins(sym):
    """All pins of a flattened symbol: list of (unit_id, pin) tuples."""
    out = []
    for u in sym.units:
        for p in u.pins:
            out.append((u.unitId, p))
    return out


def pin_xy(placed_at, rot, mirror, px, py):
    """Symbol-space pin position (Y up) -> schematic coords (Y down)."""
    x, y = px, py
    if mirror == "x":
        y = -y
    elif mirror == "y":
        x = -x
    a = math.radians(rot)
    rx = x * math.cos(a) - y * math.sin(a)
    ry = x * math.sin(a) + y * math.cos(a)
    return (round(placed_at[0] + rx, 4), round(placed_at[1] - ry, 4))


class Placed:
    """A placed symbol instance; .pin('2') gives absolute pin coords."""

    def __init__(self, sheet, sym, at, rot, mirror, unit):
        self.sheet = sheet
        self.sym = sym
        self.at = at
        self.rot = rot
        self.mirror = mirror
        self.unit = unit
        self._pins = {}
        for uid, p in _sym_pins(sym):
            if uid in (0, unit):
                self._pins[str(p.number)] = pin_xy(at, rot, mirror,
                                                   p.position.X, p.position.Y)

    def pin(self, number):
        return self._pins[str(number)]


class Sheet:
    """One .kicad_sch file."""

    def __init__(self, project, title, paper="A3"):
        self.project = project
        self.sch = Schematic.create_new()
        self.sch.version = "20230121"
        self.sch.generator = "ecu25_gen"
        self.sch.uuid = str(uuidlib.uuid4())
        self.sch.paper.paperSize = paper
        self.uuid = self.sch.uuid
        self.title = title
        self._embedded = {}
        self._refcount = {}
        self.lib = project.lib
        self._split_pts = []  # candidate wire split points (pins/labels/junctions)

    # -- symbols ----------------------------------------------------------

    def _embed(self, lib_id):
        if lib_id not in self._embedded:
            sym = self.lib.resolve(lib_id)
            self._embedded[lib_id] = sym
            self.sch.libSymbols.append(sym)
        return self._embedded[lib_id]

    def place(self, lib_id, ref, value, at, rot=0, mirror=None, unit=1,
              footprint="", show_value=True, show_ref=True,
              ref_at=None, val_at=None):
        at = (snap(at[0]), snap(at[1]))
        sym = self._embed(lib_id)
        inst = SchematicSymbol()
        inst.libId = lib_id
        inst.position = Position(X=at[0], Y=at[1], angle=rot)
        if mirror:
            inst.mirror = mirror
        inst.unit = unit
        inst.inBom = True
        inst.onBoard = True
        inst.uuid = str(uuidlib.uuid4())

        # KiCad renders property text at symbol_angle + property_angle;
        # compensate so ref/value always read horizontally.
        pa = (360 - rot) % 360
        if pa == 180:
            pa = 0  # 180 would render upside-down text; 0 reads the same
        if rot in (90, 270):
            rp = ref_at or (at[0], at[1] - 3.3)
            vp = val_at or (at[0], at[1] + 3.3)
            j = False  # centered
        else:
            rp = ref_at or (at[0] + 2.54, at[1] - 2.54)
            vp = val_at or (at[0] + 2.54, at[1] + 2.54)
            j = True
        props = [
            Property(key="Reference", value=ref, id=0,
                     position=Position(X=rp[0], Y=rp[1], angle=pa),
                     effects=font(justify_l=j, hide=not show_ref)),
            Property(key="Value", value=value, id=1,
                     position=Position(X=vp[0], Y=vp[1], angle=pa),
                     effects=font(justify_l=j, hide=not show_value)),
            Property(key="Footprint", value=footprint, id=2,
                     position=Position(X=at[0], Y=at[1], angle=0),
                     effects=font(hide=True)),
            Property(key="Datasheet", value="", id=3,
                     position=Position(X=at[0], Y=at[1], angle=0),
                     effects=font(hide=True)),
        ]
        inst.properties = props
        inst.instances = [SymbolProjectInstance(
            name=self.project.name,
            paths=[SymbolProjectPath(
                sheetInstancePath=self.project.instance_path(self),
                reference=ref, unit=unit)])]
        self.sch.schematicSymbols.append(inst)
        placed = Placed(self, self._embedded[lib_id], at, rot, mirror, unit)
        self._split_pts.extend(placed._pins.values())
        return placed

    def power(self, net, at, rot=0):
        """Place a power symbol (power:GND etc.)."""
        self.project.pwr_n += 1
        # value text sits past the symbol tip, away from the wire
        dy = 3.6 if net == "GND" else -3.6
        if rot == 180:
            dy = -dy
        return self.place(f"power:{net}", f"#PWR{self.project.pwr_n:03d}",
                          net, at, rot=rot, show_ref=False,
                          val_at=(at[0], at[1] + dy))

    # -- graphics ---------------------------------------------------------

    def wire(self, *points):
        """Polyline wire through the given points (each consecutive pair)."""
        pts = [(snap(x), snap(y)) for x, y in points]
        for a, b in zip(pts, pts[1:]):
            if a == b:
                continue
            w = Connection(type="wire",
                           points=[Position(X=a[0], Y=a[1]),
                                   Position(X=b[0], Y=b[1])])
            w.uuid = str(uuidlib.uuid4())
            self.sch.graphicalItems.append(w)

    def wire_h_then_v(self, a, b):
        self.wire(a, (b[0], a[1]), b)

    def wire_v_then_h(self, a, b):
        self.wire(a, (a[0], b[1]), b)

    def junction(self, at):
        j = Junction(position=Position(X=snap(at[0]), Y=snap(at[1])))
        j.uuid = str(uuidlib.uuid4())
        self.sch.junctions.append(j)
        self._split_pts.append((snap(at[0]), snap(at[1])))

    def no_connect(self, at):
        nc = NoConnect(position=Position(X=snap(at[0]), Y=snap(at[1])))
        nc.uuid = str(uuidlib.uuid4())
        self.sch.noConnects.append(nc)

    def label(self, at, text, rot=0):
        l = LocalLabel(text=text,
                       position=Position(X=snap(at[0]), Y=snap(at[1]), angle=rot))
        l.effects = font(justify_l=(rot in (0, 90)))
        l.uuid = str(uuidlib.uuid4())
        self.sch.labels.append(l)
        self._split_pts.append((snap(at[0]), snap(at[1])))

    def glabel(self, at, text, rot=0, shape="bidirectional"):
        g = GlobalLabel(text=text, shape=shape,
                        position=Position(X=snap(at[0]), Y=snap(at[1]), angle=rot))
        g.effects = font(justify_l=(rot in (0, 90)))
        g.uuid = str(uuidlib.uuid4())
        g.properties = [Property(
            key="Intersheetrefs", value="${INTERSHEET_REFS}", id=0,
            position=Position(X=snap(at[0]), Y=snap(at[1]), angle=0),
            effects=font(hide=True))]
        self.sch.globalLabels.append(g)
        self._split_pts.append((snap(at[0]), snap(at[1])))

    def hlabel(self, at, text, rot=0, shape="bidirectional"):
        h = HierarchicalLabel(text=text, shape=shape,
                              position=Position(X=snap(at[0]), Y=snap(at[1]), angle=rot))
        h.effects = font(justify_l=(rot in (0, 90)))
        h.uuid = str(uuidlib.uuid4())
        self.sch.hierarchicalLabels.append(h)
        self._split_pts.append((snap(at[0]), snap(at[1])))

    def text(self, at, s, size=1.7):
        t = Text(text=s, position=Position(X=at[0], Y=at[1], angle=0))
        t.effects = Effects(font=Font(height=size, width=size),
                            justify=Justify(horizontally="left"))
        t.uuid = str(uuidlib.uuid4())
        self.sch.texts.append(t)

    def _normalize_wires(self):
        """Split wire segments at pins/labels/junctions/other wire endpoints.

        kicad-cli 7 connectivity only joins wires at segment ENDPOINTS, so a
        T-connection into the middle of a long segment must physically split
        it — the same thing eeschema does while you draw interactively.
        """
        eps = 0.02
        wires = [g for g in self.sch.graphicalItems
                 if isinstance(g, Connection) and g.type == "wire"]
        others = [g for g in self.sch.graphicalItems if g not in wires]
        pts = list(self._split_pts)
        for w in wires:
            for p in w.points:
                pts.append((p.X, p.Y))

        def on_segment(p, a, b):
            ax, ay, bx, by = a.X, a.Y, b.X, b.Y
            if abs(ax - bx) < eps:  # vertical
                return (abs(p[0] - ax) < eps and
                        min(ay, by) + eps < p[1] < max(ay, by) - eps)
            if abs(ay - by) < eps:  # horizontal
                return (abs(p[1] - ay) < eps and
                        min(ax, bx) + eps < p[0] < max(ax, bx) - eps)
            return False  # diagonal wires not split (unused in this design)

        out = []
        queue = list(wires)
        while queue:
            w = queue.pop()
            a, b = w.points
            hit = next((p for p in pts if on_segment(p, a, b)), None)
            if hit is None:
                out.append(w)
                continue
            for pa, pb in ((a, Position(X=hit[0], Y=hit[1])),
                           (Position(X=hit[0], Y=hit[1]), b)):
                nw = Connection(type="wire",
                                points=[Position(X=pa.X, Y=pa.Y),
                                        Position(X=pb.X, Y=pb.Y)])
                nw.uuid = str(uuidlib.uuid4())
                queue.append(nw)
        self.sch.graphicalItems = others + out

    def save(self, path):
        self._normalize_wires()
        self.sch.to_file(path)


class Project:
    """Root schematic + child sheets + .kicad_pro."""

    def __init__(self, name, title):
        self.name = name
        self.title = title
        self.lib = LibCache()
        self.root = Sheet(self, title, paper="A3")
        self.sheets = []        # (Sheet, filename)
        self.pwr_n = 0
        self._sheet_uuid = {}   # Sheet -> hier sheet symbol uuid (in root)

    def instance_path(self, sheet):
        if sheet is self.root:
            return f"/{self.root.uuid}"
        su = self._sheet_uuid.get(id(sheet))
        # sheet symbol uuid is created in add_sheet before any place() call
        return f"/{self.root.uuid}/{su}"

    def new_sheet(self, title, filename, paper="A3"):
        s = Sheet(self, title, paper=paper)
        self._sheet_uuid[id(s)] = str(uuidlib.uuid4())
        self.sheets.append((s, filename))
        return s

    def _root_sheet_symbol(self, sheet, filename, at, size, page):
        hs = HierarchicalSheet()
        hs.position = Position(X=at[0], Y=at[1], angle=0)
        hs.width, hs.height = size
        hs.uuid = self._sheet_uuid[id(sheet)]
        hs.stroke = Stroke(width=0.1524, type="solid")
        hs.fill = ColorRGBA(R=0, G=0, B=0, A=0.0)
        hs.sheetName = Property(
            key="Sheetname", value=sheet.title, id=0,
            position=Position(X=at[0], Y=at[1] - 0.8, angle=0),
            effects=font(justify_l=True))
        hs.fileName = Property(
            key="Sheetfile", value=filename, id=1,
            position=Position(X=at[0], Y=at[1] + size[1] + 0.6, angle=0),
            effects=font(h=1.0, justify_l=True))
        hs.instances = [HierarchicalSheetProjectInstance(
            name=self.name,
            paths=[HierarchicalSheetProjectPath(
                sheetInstancePath=f"/{self.root.uuid}", page=str(page))])]
        self.root.sch.sheets.append(hs)
        return hs

    def save(self, outdir):
        import os, json
        os.makedirs(outdir, exist_ok=True)
        # root page instance
        self.root.sch.sheetInstances = [
            HierarchicalSheetInstance(instancePath="/", page="1")]
        # place sheet boxes on root in a grid
        x0, y0 = 25.4, 25.4
        w, h = 63.5, 25.4
        per_row = 4
        for i, (s, fn) in enumerate(self.sheets):
            cx = x0 + (i % per_row) * (w + 12.7)
            cy = y0 + (i // per_row) * (h + 15.24)
            self._root_sheet_symbol(s, fn, (cx, cy), (w, h), page=i + 2)
            s.save(f"{outdir}/{fn}")
        self.root.save(f"{outdir}/{self.name}.kicad_sch")
        pro = {
            "board": {"design_settings": {}},
            "libraries": {"pinned_symbol_libs": [], "pinned_footprint_libs": []},
            "meta": {"filename": f"{self.name}.kicad_pro", "version": 1},
            "schematic": {
                "annotate_start_num": 0,
                "drawing": {"default_font": "KiCad Font"},
                "legacy_lib_dir": "", "legacy_lib_list": [],
            },
            "sheets": [[self.root.uuid, ""]] + [
                [self._sheet_uuid[id(s)], s.title] for s, _ in self.sheets],
            "text_variables": {},
        }
        with open(f"{outdir}/{self.name}.kicad_pro", "w") as f:
            json.dump(pro, f, indent=2)
