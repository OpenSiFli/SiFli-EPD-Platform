# SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
"""Build actual injection, LVGL and production observers; no device access."""
from pathlib import Path
import subprocess

here = Path(__file__).resolve().parent
root = here.parents[1]
build = here / "build"
subprocess.run(["cmake", "-S", str(here), "-B", str(build), "-A", "x64"], check=True)
subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--parallel",
                "--target", "input_injection_tests", "input_injection_disabled"], check=True)
subprocess.run(["ctest", "--test-dir", str(build), "-C", "Release", "-R", "^input_injection", "-V"], check=True)
objects = list((build / "input_injection_disabled.dir").rglob("epd_input_test.obj"))
assert len(objects) == 1, objects
content = objects[0].read_bytes()
for symbol in (b"ui_pointer", b"epd_input_test_init", b"epd_input_test_submit", b"pointer_read", b"pointers", b"queue"):
    assert symbol not in content, symbol
print("PASS disabled object has no command, input functions or queue storage")
shell = (root / "src/ui/ui_shell.c").read_text(encoding="utf-8")
init = shell[shell.index("void ui_shell_init(void)"):]
assert init.index("epd_input_test_init();") < init.index("lv_indev_set_read_cb(indev, edge_pointer_read)") < init.index("ui_nav_gesture_init(")
app = (root / "src/ui/ui_app.c").read_text(encoding="utf-8")
assert "#if EPD_INPUT_TEST_ENABLED\n    epd_input_test_process_keys();\n#else\n    epd_input_process();\n#endif" in app
print("PASS production hooks: injection below observers, physical keys guarded, default branch unchanged")
