"""Measure clear-first refreshes and inspect idle peripherals over UART."""

import argparse
import json
import re
import sys
from datetime import datetime
from pathlib import Path
from time import sleep

from console import Console

ROOT = Path(__file__).resolve().parents[2]
PROMPT = r"msh [^\r\n>]*>"


def fields(reply):
    return {key: int(value) if value.isdecimal() else value
            for key, value in re.findall(r"\b(\w+)=(\w+)", reply)}


def frame(dut):
    return fields(dut.command("epd_stats", r"input_pixels=\d+[^\r\n]*[\r\n]", settle=0.06))


def wake(dut):
    dut.command("sys_wake", r"sys_wake: (requested|already active)[\r\n]", settle=0.15)


def clean_refresh(dut, count, expect_single_cycle):
    results = []
    # Keep the shell responsive while the coordinator reaches POWER_ASLEEP.
    dut.command("pm_request 1", PROMPT, settle=0.06)
    try:
        wake(dut)
        for _ in range(count):
            for _ in range(8):
                before = frame(dut)
                reply = dut.command("sys_sleep", r"sys_sleep: (sleep enabled|unavailable or busy|cannot enter lock screen|already asleep|cancelled)[\r\n]",
                                    timeout=30, settle=0.06)
                if "sys_sleep: unavailable or busy" not in reply:
                    break
                # A previous wake can still be remounting storage or resuming radio.
                sleep(1)
            if "sys_sleep: sleep enabled" not in reply:
                raise AssertionError(f"Could not complete sleep wallpaper: {reply.strip()}")
            current = frame(dut)
            if (current["frame_count"] != 85 or current["last_mode"] != "GC" or
                    current["completed"] <= before["completed"] or current["errors"] != before["errors"]):
                raise AssertionError(f"No completed INIT+GC16 refresh: {before} -> {current}")
            if expect_single_cycle and not 140 <= current["poweroff_ms"] < 230:
                raise AssertionError(f"Expected one rail discharge window: {current}")
            results.append(current)
            wake(dut)
            sleep(1)
    finally:
        try:
            wake(dut)
        finally:
            dut.command("pm_release 1", PROMPT, settle=0.06)
    return results


def counters(dut, command, prefix, names):
    pattern = prefix + r"(\s+\d+)" * len(names) + r"[\r\n]"
    return dict(zip(names, map(int, re.search(pattern, dut.command(command, pattern)).groups())))


def pm_stats(dut):
    for attempt in range(4):
        try:
            return counters(dut, "epd_pm_stats", "epd_pm",
                            ("foreground_deep", "tf_clock_parks", "wifi_clock_stop", "touch_disabled", "touch_supply"))
        except TimeoutError:
            if attempt == 3:
                raise


def touch_cycle(dut, expect_deep):
    before = pm_stats(dut)
    if before["touch_supply"] != 1 or before["touch_disabled"]:
        raise AssertionError(f"Start this case awake with touch enabled: {before}")
    expected = r"touch_pm 0[\r\n]"
    if expect_deep:
        # The board restores touch after four seconds without relying on UART
        # commands while SF32LB57 normal GPIO/UART interrupts are masked.
        dut.command("epd_touch_test 2", expected, timeout=12)
        after = pm_stats(dut)
        assert after["touch_supply"] == 1 and not after["touch_disabled"], after
        if after["foreground_deep"] <= before["foreground_deep"]:
            raise AssertionError(f"No foreground DEEP returns: {before} -> {after}")
        return {"initial": before, "restored": after}
    try:
        dut.command("epd_touch_test 0", expected)
        disabled = pm_stats(dut)
        assert disabled["touch_disabled"] == 1 and disabled["touch_supply"] == 0, disabled
    finally:
        dut.command("epd_touch_test 1", expected, timeout=6)
    restored = pm_stats(dut)
    assert restored["touch_supply"] == 1 and restored["touch_disabled"] == 0, restored
    return {"initial": before, "off": disabled, "restored": restored}


def idle_peripherals(dut):
    pm = pm_stats(dut)
    pattern = r"pm current sleep mode: ([^\r\n]+)[\r\n]"
    mode = re.search(pattern, dut.command("pm_dump", pattern))[1].strip()
    net = counters(dut, "net_idle_stats", "net_pm", ("links_up", "cyclic_timers"))
    battery_fields = ("charge_irq", "charge_valid", "usb", "status_samples", "voltage_samples")
    before = counters(dut, "battery_stats", "bat_pm", battery_fields)
    sleep(6)
    after = counters(dut, "battery_stats", "bat_pm", battery_fields)
    if not net["links_up"] and net["cyclic_timers"]:
        raise AssertionError(f"Cyclic timers still armed without a link: {net}")
    if not after["charge_irq"] or not after["charge_valid"]:
        raise AssertionError(f"Charger edge notifications unavailable: {after}")
    if before["usb"] == after["usb"] and after["status_samples"] - before["status_samples"] > 2:
        raise AssertionError(f"Unexpected battery status sampling in 6 s idle: {before} -> {after}")
    return {"pm": pm, "sleep_mode": mode, "network": net,
            "battery_before": before, "battery_after": after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=1_000_000)
    parser.add_argument("--case", choices=("clean", "idle", "touch"), action="append", required=True)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--expect-single-cycle", action="store_true")
    parser.add_argument("--expect-foreground-deep", action="store_true",
                        help="For touch case: start unlocked on a static page with radios and frontlight off")
    parser.add_argument("--output", type=Path, default=ROOT / "outputs" / "hardware-tests" /
                        datetime.now().strftime("refresh-power-%Y%m%d-%H%M%S"))
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("--samples must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    results = {}
    try:
        with (args.output / "console.log").open("wb") as log, Console(args.port, args.baud, log) as dut:
            for case in args.case:
                try:
                    if case == "clean":
                        details = clean_refresh(dut, args.samples, args.expect_single_cycle)
                    elif case == "touch":
                        details = touch_cycle(dut, args.expect_foreground_deep)
                    else:
                        details = idle_peripherals(dut)
                    results[case] = {"passed": True, "details": details}
                except (AssertionError, OSError, KeyError) as error:
                    results[case] = {"passed": False, "error": str(error)}
    except OSError as error:
        results["connect"] = {"passed": False, "error": str(error)}
    (args.output / "results.json").write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for case, result in results.items():
        print(f"{'PASS' if result['passed'] else 'FAIL'} {case}: {result}", flush=True)
    return int(not all(result["passed"] for result in results.values()))


if __name__ == "__main__":
    sys.exit(main())
