"""
route_pcb.py — autoroutes the generated ECU-25 board with freerouting.

Pipeline: pcbnew DSN export -> freerouting (headless) -> SES import ->
zone refill -> save -> DRC report. Netclass widths/clearances (set in
.kicad_pro by gen_pcb.py) ride along in the DSN, so power nets come back
wide and the HV nets keep their spacing; the .kicad_dru rules then verify
the result in DRC.

Usage:
  python3 hardware/kicad/gen/route_pcb.py <freerouting.jar> [max_passes]

Run AFTER gen_pcb.py. Re-running replaces all routing.
"""

import sys, os, subprocess
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.abspath(os.path.join(HERE, "..", "ecu25-main", "ecu25-main.kicad_pcb"))


def main():
    jar = sys.argv[1]
    passes = sys.argv[2] if len(sys.argv) > 2 else "20"
    work = os.path.dirname(PCB)
    dsn = os.path.join(work, "ecu25-main.dsn")
    ses = os.path.join(work, "ecu25-main.ses")

    board = pcbnew.LoadBoard(PCB)
    if not pcbnew.ExportSpecctraDSN(board, dsn):
        raise SystemExit("DSN export failed")
    print("exported", dsn)

    cmd = ["java", "-jar", jar, "-de", dsn, "-do", ses, "-mp", passes]
    print("running:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    if not os.path.exists(ses):
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        raise SystemExit("freerouting produced no SES")
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
