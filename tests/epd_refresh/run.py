"""Exercise the platform refresh scheduler with the V1.1 chunked display path."""

import subprocess
from pathlib import Path

here = Path(__file__).resolve().parent
build = here / "build"
subprocess.run(["cmake", "-S", str(here), "-B", str(build), "-A", "x64",
                "-DEPD_TEST_SDK_DRIVER=OFF"], check=True)
subprocess.run(["cmake", "--build", str(build), "--config", "Release",
                "--parallel", "4"], check=True)
subprocess.run(["ctest", "--test-dir", str(build), "-C", "Release",
                "--output-on-failure"], check=True)
