"""Run user-interface smoke tests on a V1.1 board using its UART console."""

import argparse
import json
import re
import sys
from datetime import datetime
from pathlib import Path
from time import monotonic, sleep

from console import Console, find_port

ROOT = Path(__file__).resolve().parents[2]
READY = r"ui_state locked=0 modal=0 center=0 navigation=0"
PROMPT = r"msh [^\r\n>]*>"
FRAME = r"epd_stats last_mode=(\w+)[^\r\n]*[\r\n]+epd_stats completed=(\d+).*? errors=(\d+)"
MODE = r"pm current sleep mode: ([^\r\n]+)"
LABEL = re.compile(r"ui_label \[(-?\d+),(-?\d+),(-?\d+),(-?\d+)\] ([^\r\n]*)")
APPS = {
    "settings": ("设置", "Settings"),
    "books": ("书架", "Books", "Bookshelf"),
    "gallery": ("图库", "Gallery"),
}
TF_PROMPT = ("是否加载 TF 卡内应用？", "Load apps from the TF card?")


def ui(dut):
    return dut.command("ui_state tree", READY, timeout=4, settle=0.15)


def label_position(tree, names):
    for x1, y1, x2, y2, label in LABEL.findall(tree):
        if label in names:
            return (int(x1) + int(x2)) // 2, (int(y1) + int(y2)) // 2
    return None


def tap(dut, position, expected=PROMPT, timeout=3):
    return dut.command(f"tp_ctrl click {position[0]} {position[1]}", expected, timeout=timeout)


def open_home(dut):
    dut.command("nav_app Main", PROMPT, settle=0.25)
    tree = ui(dut)
    if label_position(tree, TF_PROMPT):
        cancel = label_position(tree, ("取消", "Cancel"))
        if cancel is None:
            raise AssertionError("TF-card application prompt has no Cancel action")
        tap(dut, cancel)
        tree = ui(dut)
        if label_position(tree, TF_PROMPT):
            raise AssertionError("TF-card application prompt did not close")
    return tree


def frame_stats(dut):
    match = re.search(FRAME, dut.command("epd_stats", FRAME))
    return match[1], int(match[2]), int(match[3])


def require_idle(dut, timeout=3):
    mode = re.search(MODE, dut.command("pm_dump", MODE, timeout=timeout, settle=0.06))[1].strip()
    if mode != "Idle Mode":
        raise AssertionError(f"Expected Idle Mode from TEST_DEBUG=1, got {mode}")


def connect(dut):
    # The ROM can emit boot logs before the shell accepts input.
    for _ in range(4):
        try:
            require_idle(dut, timeout=5)
        except TimeoutError:
            continue
        # The shell can start while the UI and its peripheral initialization
        # are still in progress. Wait for the main UI thread before timing.
        dut.command("ui_state", READY, timeout=15)
        return
    raise TimeoutError("No power-management reply within 20 s. Confirm boot and console availability.")


def console_idle(dut):
    gaps = (2, 5, 10)
    results = []
    for seconds in gaps:
        sleep(seconds)
        start = monotonic()
        require_idle(dut)
        results.append({"idle_seconds": seconds, "reply_ms": round((monotonic() - start) * 1000, 1)})
    return results


def launch_app(dut, app):
    position = label_position(open_home(dut), APPS[app])
    _, completed, errors = frame_stats(dut)
    line = rf"switch app={app} complete ms=(\d+)"
    if position:
        reply = tap(dut, position, line, timeout=30)
        source = "touch"
    else:
        # The desktop retains its current page. The console navigation command
        # posts to the same UI-thread navigation path as a launcher tap.
        reply = dut.command(f"nav_app {app}", line, timeout=30)
        source = "navigation"
    switch_ms = int(re.search(line, reply)[1])
    deadline = monotonic() + 12
    while monotonic() < deadline:
        mode, current, failures = frame_stats(dut)
        if failures != errors:
            raise AssertionError(f"Panel reported a refresh failure while opening {app}")
        if current > completed and mode in ("APP_GC", "GC"):
            if label_position(ui(dut), APPS[app]) is None:
                raise AssertionError(f"The title for {app} is missing after screen refresh")
            return {"source": source, "switch_ms": switch_ms, "refresh_mode": mode}
        sleep(0.25)
    raise AssertionError(f"No successful panel frame after opening {app}")


CASES = ("console-idle", *APPS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    device = parser.add_argument_group("Target selection")
    device.add_argument("--port")
    device.add_argument("--usb-serial")
    device.add_argument("--usb-location")
    parser.add_argument("--baud", type=int, default=1_000_000)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if bool(args.port) == bool(args.usb_serial or args.usb_location):
        parser.error("Select --port or a stable --usb-serial / --usb-location")

    output = args.output or ROOT / "outputs" / "hardware-tests" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output.mkdir(parents=True, exist_ok=True)
    results = []
    code = 1
    try:
        port = find_port(args.port, args.usb_serial, args.usb_location)
        with (output / "console.log").open("wb") as log, Console(port, args.baud, log) as dut:
            connect(dut)
            for name in args.case or CASES:
                start = monotonic()
                try:
                    details = console_idle(dut) if name == "console-idle" else launch_app(dut, name)
                    results.append({"case": name, "seconds": round(monotonic() - start, 3),
                                    "status": "pass", "details": details})
                    print(f"PASS {name} {details}", flush=True)
                except (AssertionError, OSError) as error:
                    results.append({"case": name, "seconds": round(monotonic() - start, 3),
                                    "status": "fail", "error": str(error)})
                    print(f"FAIL {name} {error}", flush=True)
            code = int(any(result["status"] != "pass" for result in results))
    except (AssertionError, ValueError, OSError) as error:
        results.append({"case": "connect", "status": "fail", "error": str(error)})
        print(f"FAIL connect {error}", file=sys.stderr)
    (output / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Results: {output}")
    return code


if __name__ == "__main__":
    sys.exit(main())
