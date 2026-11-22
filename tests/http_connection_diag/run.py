"""Exercise production connection code with deterministic socket/TLS faults."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def extract(text, name):
    match = re.search(r"^[^\n;]*\b" + name + r"\([^;]*?\)\s*\{", text, re.M)
    assert match, name
    start = text.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def unix(path):
    text = str(Path(path).resolve()).replace("\\", "/")
    return "/mnt/" + text[0].lower() + text[2:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--before", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_path = ROOT / "modules/common/http/epd_http_core.c"
    source = source_path.read_text()
    source_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
    reader = (ROOT / "modules/common/http/epd_http_internal.h").read_text()
    reader = re.sub(r'^#include[^\n]*\n', '', reader, flags=re.M)
    names = ("epd_http_core_fail", "callback_enter", "callback_leave", "callback_cancelled",
             "epd_http_core_expired", "socket_result", "socket_send", "socket_recv", "local_route",
             "tls_connect", "deadline_ticks", "epd_http_core_perform")
    cases = ["success", "wrap", "retry", "invalid", "alloc_fail", "verify_fail", "network_dns", "timeout_send", "eof_hs"]
    cases += [prefix + stage for prefix in ("timeout_", "cancel_", "fail_") for stage in ("init", "ca", "dns", "tcp", "hs")]
    checks = []
    for variant in ("current", "module", "off", "old", "lost_origin", "lost_phase", "lost_ssl_state"):
        text = args.before.read_text() if variant == "old" else source
        if variant == "lost_origin":
            original = "(unsigned)complete, phase, diag->stage,"
            assert original in text
            text = text.replace(original, "(unsigned)complete, phase, reader->response->stage,", 1)
        if variant == "lost_phase":
            original = "unsigned diagnostic_phase = reader->phase;"
            assert original in text
            text = text.replace(original, "unsigned diagnostic_phase = 3;", 1)
        if variant == "lost_ssl_state":
            original = "diag->ssl_state = reader->tls->ssl.state;"
            assert original in text
            text = text.replace(original, "if (diag->ssl_state < 0) " + original, 1)
        folder = out / variant
        folder.mkdir(exist_ok=True)
        (folder / "reader_actual.inc").write_text(reader)
        preamble = text[text.index("#define LINE_SIZE"):text.index("bool epd_http_core_fail")]
        (folder / "core_actual.inc").write_text(preamble + "\n" + "\n".join(extract(text, n) for n in names))
        for rate in (1000, 100):
            exe = folder / f"host-{rate}"
            cmd = ["gcc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-fno-pie", "-no-pie", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   f"-DRT_TICK_PER_SECOND={rate}", f"-DEPD_INPUT_TEST_ENABLED={int(variant != 'off')}",
                   "-I" + unix(folder), "-I" + unix(ROOT / "modules/common/http"), unix(HERE / "host.c"), "-o", unix(exe)]
            if variant == "old": cmd.insert(1, "-DEXPECT_OLD")
            if variant == "module": cmd.insert(1, "-DBUILD_DLMODULE")
            process = subprocess.run(["wsl.exe", "-d", "Ubuntu", "--", "bash", "-lc", shlex.join(cmd)], capture_output=True)
            (folder / f"compile-{rate}.log").write_bytes(process.stdout + process.stderr)
            if process.returncode:
                print((process.stdout + process.stderr).decode(errors="replace"))
                raise SystemExit(process.returncode)
            selected = (["retry"] if variant == "lost_ssl_state" else
                        ["timeout_dns", "cancel_hs"] if variant.startswith("lost_") else cases)
            for case in selected:
                command = "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 " + shlex.join([unix(exe), case])
                result = subprocess.run(["wsl.exe", "-d", "Ubuntu", "--", "bash", "-lc", command], capture_output=True)
                log = result.stdout + result.stderr
                (folder / f"{rate}-{case}.log").write_bytes(log)
                negative = variant.startswith("lost_")
                assertion = {
                    "lost_origin": b"strstr(output,expected)",
                    "lost_phase": b'field("phase=")',
                    "lost_ssl_state": b"state && strtol",
                }.get(variant, b"")
                passed = (result.returncode != 0 and assertion in log) if negative else result.returncode == 0
                passed &= b"ERROR: AddressSanitizer" not in log and b"runtime error:" not in log
                checks.append(dict(variant=variant, rate=rate, case=case, negative=negative, exit=result.returncode, passed=passed))
                print(f"{'PASS' if passed else 'FAIL'} {variant} {rate} {case}", flush=True)
                if not passed: print(log.decode(errors="replace"))
    assert hashlib.sha256(source_path.read_bytes()).hexdigest() == source_hash
    report = dict(source_sha256=source_hash, checks=checks, passed=sum(c["passed"] for c in checks), count=len(checks))
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    assert all(c["passed"] for c in checks)
    print(f"PASS {len(checks)}/{len(checks)}; ASan+UBSan; source unchanged")


if __name__ == "__main__":
    main()
