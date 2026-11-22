"""Compile the production shell action dispatcher with deterministic UI gates."""
from pathlib import Path
import sys

repo, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
source = (repo / "src/ui/ui_shell.c").read_text(encoding="utf-8")
start = source.index("enum { ACTION_NONE")
parts = [source[start:source.index("};", start) + 2]]
for signature in ("static void navigation_gesture(", "static void action_process("):
    start = source.index(signature)
    parts.append(source[start:source.index("\n}", start) + 2])
(output / "shell_actions.inc").write_text("\n\n".join(parts), encoding="utf-8")
start = source.index("static void navigation_update(")
end = source.index("static lv_point_t sound_press_point;", start)
parts = parts[:2] + [source[start:end]]
start = source.index("    if (shell.screen != lv_screen_active())")
end = source.index("    if (ui_shell_locked())", start)
parts.append("static void shell_screen_process(void)\n{\n" + source[start:end] + "}")
start = source.index("static lv_obj_t *edge_create(")
parts.append(source[start:source.index("\n}", start) + 2])
(output / "shell_edges.inc").write_text("\n\n".join(parts), encoding="utf-8")
parts = []
for signature in ("static void shell_font_select(", "static void power_close("):
    start = source.index(signature)
    parts.append(source[start:source.index("\n}", start) + 2])
(output / "shell_lifecycle.inc").write_text("\n\n".join(parts), encoding="utf-8")
source = (repo / "src/ui/platform/epd_app.c").read_text(encoding="utf-8")
start = source.index("L2_RET_BSS_SECT_BEGIN(render_profile)")
(output / "render_profile_actual.inc").write_text(
    source[start:source.index("static void back_clicked", start)], encoding="utf-8")
source = (repo / "src/main.c").read_text(encoding="utf-8")
start = source.index("typedef struct { uint32_t count, total, maximum, slow; }")
(output / "ui_perf_actual.inc").write_text(
    source[start:source.index("static volatile uint8_t ui_inspection_pending", start)], encoding="utf-8")
