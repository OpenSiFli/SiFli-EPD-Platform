"""Compile the actual project PM callback file against host hardware doubles."""
import hashlib
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / "reader_prefetch"))
from cache_run import compiler_environment


def main():
    cc, env = compiler_environment(None)
    output = ROOT / "tmp/compute-pm-review"
    output.mkdir(parents=True, exist_ok=True)
    production = ROOT / "src/platform/epd_compute_pm.c"
    before = hashlib.sha256(production.read_bytes()).hexdigest()
    exe = output / "compute_pm_host.exe"
    command = [cc, "/nologo", "/TC", "/std:c11", "/utf-8", "/W4", "/WX", "/Od", "/RTC1",
               "/DEPD_COMPUTE_ACPU", "/DRT_USING_PM", "/DSF32LB57X",
               f"/I{HERE}", f"/I{ROOT / 'src/platform'}", str(production),
               str(HERE / "host.c"), f"/Fe{exe}"]
    subprocess.run(command, cwd=output, env=env, check=True, timeout=60)
    cases = subprocess.check_output([str(exe), "--list"], env=env, text=True).splitlines()
    lines = []
    for case in cases:
        result = subprocess.run([str(exe), case], cwd=output, env=env,
                                capture_output=True, text=True, timeout=15)
        lines.append(result.stdout + result.stderr)
        (output / "results.log").write_text("".join(lines), encoding="utf-8")
        print(result.stdout, end="", flush=True)
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            result.check_returncode()
    assert before == hashlib.sha256(production.read_bytes()).hexdigest(), "PM source changed during test"
    summary = f"Actual PM callbacks: {len(cases)}/{len(cases)} passed; source SHA256={before}\n"
    (output / "results.log").write_text("".join(lines) + summary, encoding="utf-8")
    print(summary, end="")


if __name__ == "__main__":
    main()
