"""Record the build, target and results of a test run."""

import json
import os
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def revision(path):
    if not (path / ".git").exists():
        return None
    return subprocess.run(["git", "rev-parse", "HEAD"], cwd=path, capture_output=True,
                          check=True, text=True).stdout.strip()


def dirty(path):
    if not (path / ".git").exists():
        return None
    return bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal",
                                        "--ignore-submodules=all"], cwd=path).strip())


class Report:
    def __init__(self, output, kind, target, firmware=None):
        self.output = output
        output.mkdir(parents=True, exist_ok=True)
        self.data = {
            "kind": kind,
            "target": target,
            "platform_commit": revision(ROOT),
            "sdk_commit": revision(ROOT / "SiFli-SDK"),
            "platform_dirty": dirty(ROOT),
            "sdk_dirty": dirty(ROOT / "SiFli-SDK"),
            "firmware_profile": firmware,
            "started_at": datetime.now(timezone.utc).isoformat(),
            "runner": {"system": platform.platform(), "python": platform.python_version(),
                       "github_run_id": os.getenv("GITHUB_RUN_ID")},
            "results": [],
        }
        self.save()

    def save(self):
        file = self.output / "results.json"
        pending = file.with_suffix(".tmp")
        pending.write_text(json.dumps(self.data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        pending.replace(file)

    def add(self, **result):
        self.data["results"].append(result)
        self.save()

    def finish(self):
        self.data["finished_at"] = datetime.now(timezone.utc).isoformat()
        self.save()
        return int(any(item["status"] != "pass" for item in self.data["results"]))
