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

import sys, os, subprocess, json, tempfile
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

    # Strip pour zones AND pour-only rule areas before the export.
    #
    # Two separate things, both of which have to go, for one reason: KiCad's
    # DSN exporter flattens intent.
    #
    # 1. A filled GND zone exports as (plane GND ...) covering the board on
    #    both layers. Pour last, not first — the router wants bare copper.
    #
    # 2. The bigger one. The rule areas on this board are POUR keepouts:
    #    GetDoNotAllowCopperPour() is true and GetDoNotAllowTracks() is
    #    false, because they exist to hold the ground pour out of the HV
    #    zone, not to stop traces. But the exporter writes every rule area as
    #    a bare (keepout ...), which Freerouting reads as "no traces, no
    #    vias". That marooned 74 cm2 of a 300 cm2 board — the whole R300-R353
    #    divider chain, the K2/K3/K4/K6 relays and the J5/J6/J8/J15 mains
    #    terminals, 98 pads sitting inside regions the router was forbidden
    #    to enter or even via out of. It is why the board would not route on
    #    two layers, and why four layers barely helped: the pads are on F.Cu
    #    and the vias that would reach an inner layer were blocked too.
    #
    # Dropping a pour keepout costs nothing during routing because the pours
    # are dropped as well. Creepage is NOT weakened by this: HV spacing is
    # enforced by the netclass clearances (HV_AC 3 mm, HV_CHAIN 1 mm), which
    # are exported as (rule (clearance ...)) and still apply. A rule area
    # that genuinely blocks tracks or vias is kept.
    #
    # The modified board is export-only and is never saved; the real file
    # keeps its zones and is refilled after the import.
    dropped_pours = dropped_keepouts = 0
    for z in list(board.Zones()):
        if not z.GetIsRuleArea():
            board.Remove(z)
            dropped_pours += 1
        elif not (z.GetDoNotAllowTracks() or z.GetDoNotAllowVias()):
            board.Remove(z)
            dropped_keepouts += 1
    print(f"withheld {dropped_pours} pour zones and {dropped_keepouts} "
          f"pour-only rule areas from the export")

    if not pcbnew.ExportSpecctraDSN(board, dsn):
        raise SystemExit("DSN export failed")
    print("exported", dsn)

    log = os.path.join(work, "route.log")

    # Freerouting 2.1.0 is a GUI program wearing a CLI, and the CLI flags are
    # not enough to make it behave. Left to itself it finishes routing, logs
    # "Auto-routing was completed", and then sits in its event loop forever
    # having written NOTHING: the session file is only flushed on a clean
    # exit. The wait below then times out, the process is killed, and the
    # entire route is lost. That is exactly how an hour-long run earlier
    # produced no output at all. Neither -dct, nor SIGTERM, nor SIGHUP
    # rescues it — all three were tried and none writes the file.
    #
    # What does work is -dct 1, but it is SLOW to take effect: the session
    # was observed appearing about 11.5 minutes after "Auto-routing was
    # completed", with the process sitting apparently idle in between. That
    # is why the wait below has to be generous — a timeout that looks
    # obviously long enough is not.
    #
    # Settings that have no command-line flag live in freerouting.json, which
    # the program reads from <java.io.tmpdir>/freerouting/. The config is
    # written here and the JVM pointed at a private temp directory, so a run
    # cannot silently inherit whatever is in the developer's own copy of that
    # file — those settings change how the board gets routed.
    #
    # gui.enabled is left TRUE on purpose. Turning it off is the documented
    # advice for headless use, but the only run tried that way was killed
    # before it finished, so it has never been seen to produce a session
    # file. The slow path is the one with evidence behind it.
    #
    # The optimizer is switched OFF, not merely single-threaded.
    #
    # Freerouting warns on every multi-threaded run that "Multi-threaded
    # route optimization is broken and it is known to generate clearance
    # violations", and -mt 1 alone did not suppress it. It is not a
    # theoretical risk here: an earlier routed board came back with 338
    # clearance violations against the 415 V rules, the worst of them
    # 2.869 mm where 3.0 mm is required — the small-margin signature of
    # traces being nudged after they were legally placed.
    #
    # Across every run so far the optimizer moved the score by 0.00, so
    # turning it off costs nothing measurable and removes the mechanism.
    # max_threads stays 1 as a second line in case a pass still runs.
    #
    # Telemetry is turned off because this board is somebody's design.
    tmp = tempfile.mkdtemp(prefix="ecu25-route-")
    cfgdir = os.path.join(tmp, "freerouting")
    os.makedirs(cfgdir, exist_ok=True)
    cfg = {
        "profile": {"allow_telemetry": False, "allow_contact": False},
        "gui": {"enabled": True, "dialog_confirmation_timeout": 1},
        "router": {"optimizer": {"max_threads": 1, "max_passes": 0}},
    }
    with open(os.path.join(cfgdir, "freerouting.json"), "w") as f:
        json.dump(cfg, f, indent=2)

    cmd = ["java", f"-Djava.io.tmpdir={tmp}", "-jar", jar,
           "-de", dsn, "-do", ses, "-mp", passes, "-mt", "1",
           "-dct", "1", "-da"]
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
    print("routed; importing session", flush=True)

    # Everything from here runs in a SUBPROCESS, and it has to.
    #
    # Removing the zones above did not just modify this board, it broke
    # pcbnew for the remainder of this process: every later LoadBoard()
    # comes back as a bare SwigPyObject with no BOARD methods on it, which
    # is the "swig/python detected a memory leak of type 'ZONE *', no
    # destructor found" warning turning into a real fault. Verified
    # directly — load, drop the zones, load again, and the second board is
    # already unusable. So the import cannot run here, even though it is
    # only a function call away.
    #
    # KiCad 7's pcbnew.ImportSpecctraSES() is no help either: it only works
    # inside the GUI, and from a script it returns False and silently does
    # nothing, which is how a "successful" routing run ends with an
    # unrouted board. import_ses.py parses the session itself.
    rc = subprocess.call([sys.executable,
                          os.path.join(HERE, "import_ses.py"), PCB, ses])
    if rc != 0:
        raise SystemExit("importing the session failed")
    print(f"saved {PCB}")

    # Check the high-voltage spacing and FAIL on it.  (Also a subprocess,
    # for the reason above.)
    #
    # This used to end by printing the path to a DRC report nobody read and
    # exiting 0 regardless, so the pipeline said "routed; saved" for a board
    # that was neither fully routed nor checked. An autorouted board is not
    # to be trusted on this point: one came back with 338 violations of the
    # 415 V rules.
    dru = os.path.splitext(PCB)[0] + ".kicad_dru"
    print(flush=True)
    rc = subprocess.call([sys.executable,
                          os.path.join(HERE, "check_hv_clearance.py"),
                          PCB, dru])
    if rc != 0:
        raise SystemExit(
            "HV clearance check FAILED — do not fabricate this board. "
            "The routed geometry violates the project's own 415 V rules.")


if __name__ == "__main__":
    main()
