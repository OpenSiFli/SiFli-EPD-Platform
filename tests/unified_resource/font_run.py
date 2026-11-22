"""Exercise production native font providers with the real LVGL/resource managers."""
import ast
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("compiler", ROOT / "tests/reader_prefetch/cache_run.py")
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)
cc, env = compiler.compiler_environment(None)
# Reuse the exact LVGL translation-unit list of the existing native-cache harness.
tree = ast.parse((ROOT / "tests/font_cache/run.py").read_text())
units = next(ast.literal_eval(node.value) for node in ast.walk(tree) if isinstance(node, ast.Assign) and
    any(isinstance(t, ast.Name) and t.id == "units" for t in node.targets))
lvgl = ROOT / "SiFli-SDK/external/lvgl_v9"
includes = [ROOT / "tests/font_cache", HERE / "stubs", lvgl, ROOT / "src/ui", ROOT / "src/platform", ROOT / "src/services"]
sources = [HERE / "font_host.c", ROOT / "src/platform/epd_resource.c", *(lvgl / "src" / name for name in units)]
with tempfile.TemporaryDirectory(prefix="font-", dir=HERE) as folder:
    folder = Path(folder)
    exe = folder / "font_host.exe"
    subprocess.run([cc, "/nologo", "/std:c11", "/utf-8", "/W3", "/Od", "/Gy",
        "/D_CRT_SECURE_NO_WARNINGS", "/DLV_CONF_INCLUDE_SIMPLE", *(f"/I{p}" for p in includes),
        *map(str, sources), f"/Fe{exe}", "/link", "/OPT:REF"], cwd=folder, env=env, check=True)
    names = subprocess.check_output([str(exe), "--list"], text=True).splitlines()
    for name in names:
        subprocess.run([str(exe), name], cwd=ROOT, env=env, check=True, timeout=30)
    print(f"Native font production resource integration: {len(names)} passed.")
