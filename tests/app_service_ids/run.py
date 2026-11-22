"""Exercise production service IDs, two-phase UI scheduling and cleanup.

Usage: python tests/app_service_ids/run.py [--case NAME] [--tick-rate HZ]
Each case runs in a fresh process against the production C source.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("compiler", ROOT / "tests/reader_prefetch/cache_run.py")
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)
RTTHREAD_HEADER = '''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define RT_NAME_MAX 8
#define RT_THREAD_PRIORITY_MAX 32
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EBUSY 2
#define RT_EINVAL 3
#define RT_ENOMEM 4
#define RT_ENOSYS 5
#define RT_WAITING_FOREVER (-1)
#define RT_EVENT_FLAG_OR 1
#define RT_EVENT_FLAG_CLEAR 2
#define RT_IPC_FLAG_FIFO 0
#define RT_IPC_FLAG_PRIO 1
#define RTM_EXPORT(symbol)
#define RT_NULL NULL
#define EPD_MEMORY_HOST
#define RT_USING_THREAD_MEM_CONTEXT
#define RT_USING_MODULE_OBJECT_CONTEXT
typedef int rt_base_t;
typedef int rt_err_t;
typedef uint32_t rt_uint32_t;
typedef int32_t rt_int32_t;
typedef struct rt_thread *rt_thread_t;
struct rt_thread {
    char name[RT_NAME_MAX];
    void *parameter;
    void (*entry)(void *);
    void (*cleanup)(rt_thread_t);
};
struct rt_event { uint32_t bits; };
struct rt_mutex { bool held; };
struct rt_object_global_scope { bool active; };
'''


def owner_sources(build):
    for source, destination in [('src/platform/epd_memory_manager.c', 'memory_core.inc'),
                                ('src/services/app_background.c', 'background.inc')]:
        text = (ROOT / source).read_text(encoding='utf-8')
        (build / destination).write_text(re.sub(r'^#include[^\n]*\n', '', text, flags=re.M),
                                         encoding='utf-8')
    (build / 'owner_host.h').write_text((HERE / 'owner_host.h').read_text(encoding='utf-8'), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append", help="Run a named case; repeat to select more.")
    parser.add_argument("--tick-rate", action="append", type=int,
                        help="Ticks per second; defaults to both 1000 and 100.")
    args = parser.parse_args()
    tick_rates = args.tick_rate or [1000, 100]
    if any(rate <= 0 or rate > 1000000 for rate in tick_rates):
        parser.error("--tick-rate must be between 1 and 1000000")
    cc, env = compiler.compiler_environment(None)
    failures = []
    total = 0
    with tempfile.TemporaryDirectory(prefix="service-ids-") as folder:
        build = Path(folder)
        owner_sources(build)
        source = (ROOT / "src/services/app_service.c").read_text(encoding="utf-8")
        (build / "service.inc").write_text(
            re.sub(r'^#include[^\n]*\n', '', source, flags=re.M), encoding="utf-8")
        (build / "rtthread.h").write_text(RTTHREAD_HEADER, encoding="utf-8")
        for rate in tick_rates:
            exe = build / f"service_ids_{rate}.exe"
            command = [cc, "/nologo", "/std:c11", "/utf-8", "/W3", "/WX", "/TC", "/Od", "/RTC1",
                       "/D_CRT_SECURE_NO_WARNINGS", f"/DRT_TICK_PER_SECOND={rate}",
                       f"/I{build}", f"/I{ROOT / 'src/services'}", f"/I{ROOT / 'src/platform'}",
                       str(HERE / "host.c"), f"/Fe{exe}"]
            subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
            available = subprocess.check_output(
                [str(exe), "--list"], cwd=build, env=env, text=True, timeout=15).splitlines()
            selected = args.case or available
            unknown = set(selected) - set(available)
            if unknown:
                parser.error(f"Unknown cases: {', '.join(sorted(unknown))}")
            for name in selected:
                total += 1
                print(f"RUN {name} (ticks/s={rate})", flush=True)
                try:
                    subprocess.run([str(exe), name], cwd=build, env=env, check=True, timeout=15)
                except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                    failures.append(f"{name}@{rate}")
                    print(f"FAIL {name}@{rate}: {error}", flush=True)
    print(f"Service IDs and scheduling: {total - len(failures)}/{total} cases passed.", flush=True)
    if failures:
        print("Failed cases: " + ", ".join(failures), flush=True)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
