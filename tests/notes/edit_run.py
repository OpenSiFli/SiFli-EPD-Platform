"""Run transactional text edits from the runtime with allocation/encoding counters."""
from pathlib import Path
import os
import subprocess
import tempfile
from notebook_run import between, compiler_environment

repo = Path(__file__).resolve().parents[2]
compiler, environment = compiler_environment()
with tempfile.TemporaryDirectory(prefix="notes-edit-") as directory:
    output = Path(directory)
    (output / "app_work.inc").write_text((repo / "modules/common/app_work.h").read_text(encoding="utf-8").replace('#include "app_service.h"', ''), encoding="utf-8")
    runtime = (repo / "modules/notes/notes_runtime.c").read_text(encoding="utf-8")
    (output / "edit_actual.inc").write_text(between(runtime, "static void history_clear(", "bool notes_document("), encoding="utf-8")
    executable = output / ("edit.exe" if os.name == "nt" else "edit")
    includes = [output, repo / "modules/notes"]
    if os.name == "nt":
        command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W4", "/DNOTES_HOST", "/D_CRT_SECURE_NO_WARNINGS"]
        command += [f"/I{p}" for p in includes] + [str(repo / "tests/notes/edit_test.c"), f"/Fe:{executable}"]
    else:
        command = [compiler, "-std=c11", "-DNOTES_HOST", "-Wall", "-Wextra"]
        command += [f"-I{p}" for p in includes] + [str(repo / "tests/notes/edit_test.c"), "-o", str(executable)]
    subprocess.run(command, cwd=output, env=environment, check=True)
    subprocess.run([str(executable)], cwd=output, check=True)
