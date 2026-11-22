"""Prepare the app UI and platform keyboard for the LVGL host harness."""
import re
import sys
from pathlib import Path

repo, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
source = (repo / "modules/notes/notes_ui.c").read_text(encoding="utf-8")
source = source[source.index("typedef enum { LIST"):source.index("static void page_message(")]
(output / "actual_ui.inc").write_text(source, encoding="utf-8")
runtime = (repo / "modules/notes/notes_runtime.h").read_text(encoding="utf-8")
runtime = re.sub(r'#include "(app_service|epd_ai|epd_record)\.h"', "", runtime)
types = "typedef int epd_speech_error_t; typedef int epd_background_t;\n"
(output / "runtime_actual.h").write_text(types + runtime, encoding="utf-8")
keyboard = (repo / "src/ui/platform/epd_keyboard.c").read_text(encoding="utf-8")
keyboard = re.sub(r"^#include.*$", "", keyboard, flags=re.M)
for name in ("button", "icon_button", "render", "instance", "identity", "visible", "enable"):
    keyboard = re.sub(r"\b" + name + r"\b", "host_kb_" + name, keyboard)
(output / "keyboard_actual.inc").write_text(keyboard, encoding="utf-8")
navigation = (repo / "src/ui/platform/epd_input.c").read_text(encoding="utf-8")
def function(signature):
    start = navigation.index(signature)
    return navigation[start:navigation.index("\n}", start) + 2]
guard = navigation.index("typedef struct\n{\n    epd_memory_owner_t owner, previous;")
guard_end = navigation.index("} input_guard_t;", guard) + len("} input_guard_t;")
(output / "navigation_actual.inc").write_text(navigation[guard:guard_end] + "\n\n" +
    "\n\n".join(function(name) for name in (
        "static bool context_open(", "static bool context_enter(", "static void context_leave(",
        "static bool callback_enter(", "static void publish_scope(",
        "bool epd_input_focusable(", "void epd_input_set_ops(", "void epd_input_back(")), encoding="utf-8")
