"""Real public HTTP/speech boundaries, with transport/engine fault doubles."""
import subprocess
import sys
from pathlib import Path
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment
sys.path.insert(0, str(ROOT / "tests/music_progress"))
from run import extract
cc, env = compiler_environment("cl")
out = ROOT / "tmp/business_memory/entries"
out.mkdir(parents=True, exist_ok=True)
http = ROOT / "modules/common/http"
base = (http / "epd_http.c").read_text()
core = (http / "epd_http_core.c").read_text()
(out / "http_growth_actual.inc").write_text(extract(core, "body_reserve"))
(out / "http_callbacks_actual.inc").write_text("\n".join(extract(core, n) for n in (
    "callback_enter", "callback_leave", "callback_cancelled", "sent_callback", "deliver", "header_callback")))
(out / "http_entry_actual.inc").write_text("\n".join(extract(base, n) for n in (
    "epd_http_response_clear", "epd_http_perform")) + "\n" + extract(core, "epd_http_stream_perform"))
speech = (ROOT / "src/services/speech/speech_core.c").read_text()
(out / "speech_actual.inc").write_text("\n".join(extract(speech, n) for n in (
    "destroy", "run_kind", "epd_speech_start")))
capture = (ROOT / "src/services/speech/speech_audio.c").read_text().split("\n#else\nbool speech_audio_input_available")[0]
(out / "input_actual.inc").write_text("\n".join(extract(capture, n) for n in (
    "input_cleanup", "input_open", "speech_audio_input_open_limited", "speech_audio_input_close")))
(out / "flac_actual.inc").write_text(extract((ROOT / "modules/music/music_flac_worker.c").read_text(), "audio_run"))
includes = [out, HERE / "stubs", ROOT / "tests/unified_resource/stubs", ROOT / "src/platform", http,
            ROOT / "src/services/ai"]
common = [ROOT / "src/platform" / s for s in (
    "epd_memory_manager.c", "epd_resource.c", "epd_resource_runtime.c", "epd_activity_work.c")]
for group, cases in (("http", ("success", "transport_failure", "gzip_failure", "cancelled", "stream",
                             "sse_large", "sse_credit", "sse_limited", "sse_closed", "callback_cancel", "nested")),
                     ("speech", ("asr", "tts", "invalid", "start_failure", "closed")),
                     ("input", ("success", "thread_failure", "start_failure", "closed")),
                     ("flac", ("success", "open_failure", "close_failure"))):
    if len(sys.argv) > 1 and group not in sys.argv[1:]: continue
    exe = out / f"{group}.exe"
    extra = [ROOT / "src/services/ai" / s for s in ("ai_memory.c", "ai_sse.c")] if group == "http" else []
    subprocess.run([cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", "/wd4200", "/wd4389",
                    "/Od", "/Zi", "/fsanitize=address", "/DEPD_MEMORY_HOST", "/DRT_USING_MEMHEAP_OWNER",
                    *(f"/I{p}" for p in includes), str(HERE / f"{group}_host.c"), *map(str, common + extra),
                    f"/Fe{exe}"], cwd=out, env=env, check=True, timeout=60)
    for case in cases:
        subprocess.run([str(exe), case], cwd=out, env=env, check=True, timeout=15)
if len(sys.argv) == 1 or "http" in sys.argv[1:]:
    for name, flags in (("off", ["/DBUSINESS_NO_CONTEXT"]), ("context", []),
                        ("owner", ["/DBUSINESS_NO_CONTEXT", "/DRT_USING_MEMHEAP_OWNER"])):
        exe = out / f"http_disabled_{name}.exe"
        subprocess.run([cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", *flags,
                        *(f"/I{p}" for p in includes), str(HERE / "http_disabled_host.c"),
                        f"/Fe{exe}"], cwd=out, env=env, check=True, timeout=60)
        subprocess.run([str(exe)], cwd=out, env=env, check=True, timeout=15)
