"""Compile the complete control center against real LVGL and service doubles."""
from pathlib import Path
import sys

repo, output = map(Path, sys.argv[1:])
source = (repo / "src/ui/ui_control_center.c").read_text(encoding="utf-8")
output.mkdir(parents=True, exist_ok=True)
(output / "control_center_actual.inc").write_text(
    "\n".join(line for line in source.splitlines() if not line.startswith("#include")),
    encoding="utf-8",
)
