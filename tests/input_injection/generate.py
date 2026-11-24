# SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
"""Copy production shell observers and lock callbacks verbatim for real LVGL."""
from pathlib import Path
import re
import sys


def function(text, name):
    masked = re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda match: " " * len(match[0]), text)
    match = re.search(rf"^[A-Za-z_][\w* \t]*\b{name}\s*\([^;{{}}]*\)\s*\{{", masked, re.M)
    if not match:
        raise ValueError(name)
    end, depth = match.end(), 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return text[match.start():end]


repo, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
shell = (repo / "src/ui/ui_shell.c").read_text(encoding="utf-8")
start = shell.index("static struct {\n    lv_indev_read_cb_t read;")
end = shell.index("} edge_pointer;", start) + len("} edge_pointer;")
parts = [function(shell, "navigation_update"), shell[start:end], function(shell, "edge_pointer_read")]
lock = (repo / "src/ui/apps/app_lock.c").read_text(encoding="utf-8")
start = lock.index("typedef struct\n{")
end = lock.index("} lock_state_t;", start) + len("} lock_state_t;")
parts += [re.search(r"^#define LOCK_SUMMARY_COUNT.*$", lock, re.M)[0],
          re.search(r"^#define LOCK_HOLD_MS.*$", lock, re.M)[0], lock[start:end]]
parts += [function(lock, name) for name in ("lock_state", "lock_cancel_hold", "lock_finish_hold", "lock_unlock_event")]
(output / "actual_handlers.inc").write_text("\n\n".join(parts), encoding="utf-8")
