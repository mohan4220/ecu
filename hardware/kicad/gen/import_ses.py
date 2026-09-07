"""
import_ses.py — load a Specctra .ses session into a KiCad board, headless.

KiCad 7's pcbnew Python module exposes ImportSpecctraSES(filename), but it
operates on the GUI's current board: called from a standalone script it
returns False and does nothing (verified on 7.0.11). That left the routing
pipeline needing a manual File -> Import -> Specctra Session step, which is
exactly the kind of thing that silently stops happening. So this parses the
session itself.

Format notes, from KiCad's own DSN/SES round trip:
  - (resolution um 10) means one unit is 1/10 um, i.e. 100 nm, which is what
    pcbnew uses internally anyway.
  - The session's Y axis points UP and KiCad's points DOWN, so every Y is
    negated. Getting this wrong mirrors the whole board about the top edge
    and still "imports cleanly", which is why the check at the end compares
    track endpoints against the pads they are supposed to land on.
  - A via padstack is named like "Via[0-1]_800:400_um" — diameter 800 um,
    drill 400 um.

Usage:
  python3 import_ses.py <board.kicad_pcb> <session.ses>   # system python
"""

import os
import re
import sys

import pcbnew

SCALE_NM = 100  # one SES unit at (resolution um 10)


def tokenize(text):
    """
    Quoted atoms have to be one token. Net names here look like
    "/AC sensing/ACV_GEN_L1" — they contain spaces, and splitting on
    whitespace turns one net into four tokens that match nothing on the
    board.
    """
    return [t.strip('"')
            for t in re.findall(r'\(|\)|"[^"]*"|[^\s()"]+', text)]


def parse(tokens, pos=0):
    """S-expression to nested lists."""
    out = []
    while pos < len(tokens):
        t = tokens[pos]
        if t == "(":
            sub, pos = parse(tokens, pos + 1)
            out.append(sub)
        elif t == ")":
            return out, pos + 1
        else:
            out.append(t)
            pos += 1
    return out, pos


def find(node, key):
    for item in node:
        if isinstance(item, list) and item and item[0] == key:
            yield item


def resolution_scale(session):
    """nm per session unit, from (resolution um 10)."""
    for res in find(session, "resolution"):
        if len(res) >= 3 and res[1] == "um":
            return 1000.0 / float(res[2])
    return float(SCALE_NM)


def import_ses(pcb_path, ses_path):
    board = pcbnew.LoadBoard(pcb_path)

    layers = {}
    for i in range(pcbnew.PCB_LAYER_ID_COUNT):
        if pcbnew.IsCopperLayer(i):
            layers[board.GetLayerName(i)] = i
            layers[board.GetStandardLayerName(i)] = i

    nets = {n: board.FindNet(n).GetNetCode()
            for n in [str(x) for x in board.GetNetsByName().keys()]}

    session, _ = parse(tokenize(open(ses_path, encoding="utf-8").read()))
    session = session[0]
    scale = resolution_scale(session)

    routes = next(find(session, "routes"), None)
    if routes is None:
        raise SystemExit("no (routes) section in the session")
    network = next(find(routes, "network_out"), None)
    if network is None:
        raise SystemExit("no (network_out) section in the session")

    # Clear any previous routing so a re-import cannot double up.
    for t in list(board.GetTracks()):
        board.Remove(t)

    added_tracks = added_vias = 0
    unknown_nets = set()

    for net in find(network, "net"):
        name = net[1]
        if name not in nets:
            unknown_nets.add(name)
            continue
        code = nets[name]

        for wire in find(net, "wire"):
            path = next(find(wire, "path"), None)
            if path is None:
                continue
            layer_name = path[1]
            if layer_name not in layers:
                raise SystemExit(f"session names an unknown layer {layer_name}")
            layer = layers[layer_name]
            width = int(round(float(path[2]) * scale))
            coords = [float(v) for v in path[3:]]
            pts = [(int(round(coords[i] * scale)),
                    int(round(-coords[i + 1] * scale)))
                   for i in range(0, len(coords) - 1, 2)]
            for a, b in zip(pts, pts[1:]):
                if a == b:
                    continue
                t = pcbnew.PCB_TRACK(board)
                t.SetStart(pcbnew.VECTOR2I(a[0], a[1]))
                t.SetEnd(pcbnew.VECTOR2I(b[0], b[1]))
                t.SetWidth(width)
                t.SetLayer(layer)
                t.SetNetCode(code)
                board.Add(t)
                added_tracks += 1

        for via in find(net, "via"):
            m = re.search(r"(\d+):(\d+)_um", via[1])
            dia = int(round(float(m.group(1)) * 1000)) if m else 800000
            drill = int(round(float(m.group(2)) * 1000)) if m else 400000
            x = int(round(float(via[2]) * scale))
            y = int(round(-float(via[3]) * scale))
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I(x, y))
            v.SetWidth(dia)
            v.SetDrill(drill)
            v.SetViaType(pcbnew.VIATYPE_THROUGH)
            v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            v.SetNetCode(code)
            board.Add(v)
            added_vias += 1

    if unknown_nets:
        raise SystemExit(f"session names nets not on the board: "
                         f"{sorted(unknown_nets)[:5]}")

    check_alignment(board)

    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(pcb_path, board)
    unconn = board.GetConnectivity().GetUnconnectedCount(True)
    print(f"imported {added_tracks} tracks, {added_vias} vias")
    print(f"unconnected after import: {unconn}")
    return board


def check_alignment(board):
    """
    A mirrored import still looks like a routed board, so prove the geometry
    actually landed on copper. Every routed net must have at least one track
    end sitting on one of its own pads; a sign error on Y pushes that
    distance into the hundreds of millimetres for every net at once, which is
    what makes a cheap sample conclusive.
    """
    pads = {}
    for fp in board.GetFootprints():
        for p in fp.Pads():
            pads.setdefault(p.GetNetCode(), []).append(p)

    best_per_net = {}
    for t in board.GetTracks():
        code = t.GetNetCode()
        if code not in pads:
            continue
        for end in (t.GetStart(), t.GetEnd()):
            d = min((p.GetPosition() - end).EuclideanNorm()
                    for p in pads[code])
            if code not in best_per_net or d < best_per_net[code]:
                best_per_net[code] = d

    if not best_per_net:
        return
    worst = max(best_per_net.values()) / 1e6
    if worst > 5.0:
        raise SystemExit(
            f"import looks mirrored or mis-scaled: on one net the closest a "
            f"track end gets to a pad of that net is {worst:.1f} mm")
    print(f"alignment check: {len(best_per_net)} routed nets, worst "
          f"track-end-to-own-pad distance {worst:.3f} mm")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    pcb, ses = sys.argv[1], sys.argv[2]
    if not os.path.exists(ses):
        raise SystemExit(f"no session file: {ses}")
    import_ses(pcb, ses)


if __name__ == "__main__":
    main()
