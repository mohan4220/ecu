"""
route_pcb.py — autoroutes the generated ECU-25 board with freerouting.

Pipeline: pcbnew DSN export -> freerouting (headless) -> SES import ->
zone refill -> save -> DRC report. Netclass widths/clearances (set in
.kicad_pro by gen_pcb.py) ride along in the DSN, so power nets come back
wide and the HV nets keep their spacing; the .kicad_dru rules then verify
the result in DRC.

Usage:
  python3 hardware/kicad/gen/route_pcb.py <freerouting.jar> [passes] [timeout_s]

Run AFTER gen_pcb.py. Re-running replaces all routing.

Notes from actually doing this:
  - freerouting output is STREAMED to ecu25-main/route.log, not captured.
    Capturing it means you learn nothing until the run ends, and a run that
    is going nowhere looks identical to one that is nearly done.
  - It needs Java 21 or older for 2.1.0; the 2.4.x jars are built for 25.
  - Passes are not cheap. Each one re-rips and re-routes, and this board
    (2 layers, ~480 airwires, 3 mm clearance around the 415 V nets) takes
    the better part of an hour for the first few. Start with 3, look at the
    result, then decide whether more passes are worth the time.
"""

import sys, os, subprocess
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.abspath(os.path.join(HERE, "..", "ecu25-main", "ecu25-main.kicad_pcb"))


def main():
    jar = sys.argv[1]
    passes = sys.argv[2] if len(sys.argv) > 2 else "3"
    timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 7200
    work = os.path.dirname(PCB)
    dsn = os.path.join(work, "ecu25-main.dsn")
    ses = os.path.join(work, "ecu25-main.ses")

    board = pcbnew.LoadBoard(PCB)
    if not pcbnew.ExportSpecctraDSN(board, dsn):
        raise SystemExit("DSN export failed")
    print("exported", dsn)

    log = os.path.join(work, "route.log")
    cmd = ["java", "-jar", jar, "-de", dsn, "-do", ses, "-mp", passes]
    print("running:", " ".join(cmd))
    print("progress ->", log, flush=True)
    with open(log, "w") as lf:
        p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT,
                             text=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            raise SystemExit(
                f"freerouting still running after {timeout}s — killed. "
                f"See {log}; try fewer passes.")
    if not os.path.exists(ses):
        raise SystemExit(f"freerouting produced no SES; see {log}")
    print("routed; importing session")

    if not pcbnew.ImportSpecctraSES(board, ses):
        raise SystemExit("SES import failed")
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(PCB, board)

    unconn = board.GetConnectivity().GetUnconnectedCount(True)
    print(f"saved {PCB}")
    print(f"tracks: {len(board.GetTracks())}, unconnected: {unconn}")

    rpt = os.path.join(work, "drc.rpt")
    pcbnew.WriteDRCReport(board, rpt, pcbnew.EDA_UNITS_MILLIMETRES, False)
    print("DRC report:", rpt)


if __name__ == "__main__":
    main()
