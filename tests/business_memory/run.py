"""Exercise the production admission helper against the real resource/core code."""
import subprocess
import sys
from pathlib import Path
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment
cc, env = compiler_environment("cl")
out = ROOT / "tmp/business_memory/host"
out.mkdir(parents=True, exist_ok=True)
includes = [HERE / "stubs", ROOT / "tests/unified_resource/stubs", ROOT / "src/platform"]
sources = [HERE / "activity_host.c", *(ROOT / "src/platform" / s for s in (
    "epd_memory_manager.c", "epd_resource.c", "epd_resource_runtime.c", "epd_activity_work.c"))]
for rate in (1000, 100):
    exe = out / f"activity_{rate}.exe"
    subprocess.run([cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", "/wd4200", "/wd4389",
                    "/Od", "/Zi", "/fsanitize=address", "/DEPD_MEMORY_HOST", "/DRT_USING_MEMHEAP_OWNER",
                    f"/DRT_TICK_PER_SECOND={rate}", *(f"/I{p}" for p in includes),
                    *map(str, sources), f"/Fe{exe}"], cwd=out, env=env, check=True, timeout=60)
    for case in sys.argv[1:] or ("lifecycle", "timeout", "sram_reject", "closed", "seed_failure", "restricted_lanes", "parent_closed_live"):
        subprocess.run([str(exe), case], cwd=out, env=env, check=True, timeout=15)
for name, flags in (("off", ["/DBUSINESS_NO_CONTEXT"]), ("context", []),
                    ("owner", ["/DBUSINESS_NO_CONTEXT", "/DRT_USING_MEMHEAP_OWNER"])):
    exe = out / f"disabled_{name}.exe"
    subprocess.run([cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", *flags,
                    *(f"/I{p}" for p in includes), str(HERE / "disabled_host.c"),
                    str(ROOT / "src/platform/epd_activity_work.c"), f"/Fe{exe}"], cwd=out, env=env, check=True, timeout=60)
    subprocess.run([str(exe)], cwd=out, env=env, check=True, timeout=15)
