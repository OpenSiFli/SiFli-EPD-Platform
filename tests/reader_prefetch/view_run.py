"""Run extracted production reader-view scheduling with the real kernel/cache.

python tests/reader_prefetch/view_run.py [--case NAME] [--cc COMPILER]
Function bodies and reader state declarations are extracted unchanged on every
run. Document I/O, compute transport, font metrics and UI presentation use doubles.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from cache_run import compiler_environment


FUNCTIONS = (
    "compute_cancel fail cursor_before cursor_equal preview_visible font_slot font_for fonts_ready page_fonts_ready width_available "
    "page_reset preview_start reserve_pages add_toc utf8_letter decode_at page_step "
    "target_needed elapsed_ms trace_start cache_contains cache_in_window cache_sync image_page_nearby "
    "cache_store cache_candidate_in_range cache_candidate_allowed cache_candidate cache_warm_candidate cache_warm cache_target_pending "
    "text_turn trace_work page_runnable preview_back_discard preview_back_stop preview_back_begin preview_back_seek_step preview_back_finish preview_cached_target preview_candidate preview_prefetch_finish "
    "preview_back restore_target index_finish link_process page_read_failed target_finish compute_one "
    "target_runnable compute_runnable core_seek core_turn core_seek_cursor "
    "core_cancel_pending core_waiting core_background_pause core_step core_set_error_code publish_state "
    "trace_ready index_cancel index_client_cleanup index_client_begin index_client_end index_checksum index_validate index_size index_load index_save images_prepare "
    "core_release state_reflow epd_reader_page_text duration_ms cursor_at_segment_start "
    "page_error page_target indexed_pages sparse_compact sparse_seek_page layout_accept turn_continue"
).split()


def masked(source):
    # Ignore braces in comments/literals while preserving every source offset.
    pattern = r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: "".join("\n" if c == "\n" else " " for c in m[0]), source)


def extract(source, name):
    clean = masked(source)
    pattern = rf"^[A-Za-z_][\w* \t]*\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{"
    matches = list(re.finditer(pattern, clean, re.MULTILINE))
    if len(matches) != 1:
        raise RuntimeError(f"Expected one definition of {name}, found {len(matches)}")
    match = matches[0]
    opening = match.end() - 1
    depth = 1
    end = opening + 1
    while depth and end < len(clean):
        depth += (clean[end] == "{") - (clean[end] == "}")
        end += 1
    if depth:
        raise RuntimeError(f"Unclosed definition: {name}")
    return match.start(), source[match.start():opening].strip() + ";", source[match.start():end]


def generate(root, build, regression=None, probe_backward_index=False):
    reader = root / "modules/common/reader"
    source_path = reader / "reader_view.c"
    source = source_path.read_text(encoding="utf-8")
    if regression in ("images", "save", "both"):
        # Mutate only a temporary source copy; never write to reader_view.c.
        offset, _, step = extract(source, "core_step")
        gate = "if (!view.background_paused && view.status.ready && !core_waiting())"
        save_call = "            index_save(false);"
        if step.count(gate) != 1 or step.count(save_call) != 1:
            raise RuntimeError("Optional-I/O gate changed; review regression mutations")
        if regression in ("images", "both"):
            step = step.replace(gate, "if ((view.display_pending && images_ready(view.building)) || "
                                "(view.status.ready && !core_waiting()))")
            if regression == "images":
                step = step.replace(save_call,
                                    "            if (view.status.ready && !core_waiting()) index_save(false);")
        if regression in ("save", "both"):
            step = step.replace(save_call, "")
            marker = "        if (VIEW_BACKGROUND_TRACE && view.cache_logged_count"
            if step.count(marker) != 1:
                raise RuntimeError("Index-save insertion point changed")
            step = step.replace(marker, "        index_save(false);\n" + marker)
        _, _, original = extract(source, "core_step")
        source = source[:offset] + step + source[offset + len(original):]
        if regression in ("save", "both"):
            # Restore old lack of target protection at both layers, keeping
            # partial serialization, throttling and failure handling intact.
            offset, _, save = extract(source, "index_save")
            old = save
            guard = "    if (!force && core_waiting()) return;\n"
            if save.count(guard) != 1 or save.count(" || !view.status.ready") != 1:
                raise RuntimeError("Index-save target protection changed")
            save = save.replace(guard, "").replace(" || !view.status.ready", "")
            source = source[:offset] + save + source[offset + len(old):]

    mutations = {
        "pause-ignored": (
            "    view.background_paused = paused;\n", "    view.background_paused = false;\n"),
        "pause-no-idle": (
            "        reader_compute_idle();\n        if (view.document)",
            "        if (view.document)"),
        "glyph-single": (
            "if (view.glyph_steps >= VIEW_GLYPH_BURST) view.glyph_turn = false;",
            "view.glyph_turn = false;"),
        "glyph-count-unbounded": (
            "if (view.glyph_steps >= VIEW_GLYPH_BURST) view.glyph_turn = false;", ""),
        "glyph-time-unbounded": (
            "if (view.glyph_ticks >= (limit ? limit : 1)) view.glyph_turn = false;", "(void)limit;"),
        "bind-idle": (
            "bool present = foreground && commit && core_waiting() && !view.notified;",
            "bool present = foreground && commit && core_waiting();"),
        "warm-gate": (
            "if (!view.allow_glyphs || (core_waiting() && !view.display_pending)) return NULL;",
            "if (!view.allow_glyphs || core_waiting()) return NULL;"),
    }
    if regression in mutations:
        old, new = mutations[regression]
        if source.count(old) != 1:
            raise RuntimeError(f"Scheduling gate changed; review {regression} mutation")
        source = source.replace(old, new)
    if regression == "preview-metadata-only":
        # Compare the new dispatch with the previous metadata-only path without
        # changing production or bypassing any scheduler state transitions.
        call = re.compile(r'epd_app_font_prepare_text_preview\(font_for\(p->record.style\),\s*'
                          r'p->data \+ p->cursor.byte,\s*p->record.length - p->cursor.byte\)')
        source, count = call.subn('epd_app_font_prepare_text(font_for(p->record.style), '
                                  'p->data + p->cursor.byte, p->record.length - p->cursor.byte, false)', source)
        if count != 1:
            raise RuntimeError("Expected one preview preparation call")
    preview_mutations = {
        "preview-requires-chapters": ("preview_back_begin",
            "    uint32_t window = view.display->bytes > VIEW_BACK_WINDOW_MAX / 4 ?",
            "    document_chapter_range_t required;\n"
            "    if (!document_chapter_range(view.document, end.record, &required) || !required.indexed) return false;\n"
            "    uint32_t window = view.display->bytes > VIEW_BACK_WINDOW_MAX / 4 ?"),
        "preview-cache-required": ("preview_back_finish",
            "if (target && cursor_equal(next, view.preview_back_end) && page->count)",
            "if (!view.preview_back_blocked && target && cursor_equal(next, view.preview_back_end) && page->count)"),
        "preview-global-first": ("target_finish", "        if (preview_cached_target()) return;",
            "        if (view.count && (view.complete || !cursor_before(view.pages[view.count], view.anchor))) {\n"
            "            preview_back(view.anchor); view.turn_pending = 0; return;\n"
            "        }\n        if (preview_cached_target()) return;"),
        "preview-clip-unchecked": ("preview_cached_target",
            "        if (view.preview_clipped && !cursor_equal(end, view.preview_end)) continue;", ""),
        "preview-turn-link-stale": ("core_turn",
            "    view.link_target[0] = 0; view.link_scan = 0;", ""),
        "preview-refill-starved": ("core_turn",
            "    if (direction > 0 && view.preview_back_scanning) preview_back_stop(false);\n", ""),
    }
    if regression in preview_mutations:
        name, old, new = preview_mutations[regression]
        offset, _, body = extract(source, name)
        if body.count(old) != 1:
            raise RuntimeError(f"Preview boundary changed; review {regression} mutation")
        source = source[:offset] + body.replace(old, new) + source[offset + len(body):]
    if regression == "paused-index-stall":
        offset, _, body = extract(source, "compute_one")
        old = body
        start = body.index("    if (!view.complete && view.index->done")
        end = body.index("    bool pending = view.display_pending;", start)
        body = body[:start] + body[end:]
        source = source[:offset] + body + source[offset + len(old):]
    if regression == "reverse-quadratic":
        offset, _, body = extract(source, "cache_candidate")
        old = body
        start = body.index("    unsigned end = view.wanted > 3")
        body = body[:start] + """    for (unsigned n = view.wanted > 3 ? view.wanted - 3 : 0; n; ) {
        --n;
        if (view.pages[n + 1].record < view.cache_begin) break;
        if (cache_candidate_allowed(n)) return n;
    }
    return UINT_MAX;
}"""
        source = source[:offset] + body + source[offset + len(old):]
    if probe_backward_index:
        offset, _, body = extract(source, "compute_one")
        marker = "    if (view.preview && view.preview_cache_active && (preview_visible() || view.turn_pending < 0) &&"
        if body.count(marker) != 1:
            raise RuntimeError("Backward dispatch changed; review temporary probe")
        replacement = ("    if (view.preview && view.turn_pending < 0 && !view.preview_back_scanning &&\n"
                       "        view.count && (view.complete || !cursor_before(view.pages[view.count], view.anchor)))\n"
                       "        restore_target();\n" + marker)
        source = source[:offset] + body.replace(marker, replacement) + source[offset + len(body):]
    if regression or probe_backward_index:
        source_path = build / ("reader_view_probe.c" if probe_backward_index else "reader_view_old.c")
        source_path.write_text(source, encoding="utf-8")
    start = source.index("#define VIEW_TEXT")
    end = source.index("static unsigned elapsed_ms")
    functions = sorted(extract(source, name) for name in FUNCTIONS)
    data_state = '\n'.join(re.findall(r'^static (?:epd_reading_data_t data_client[^;]*|bool data_retiring[^;]*);', source, re.MULTILINE))
    if 'data_active' not in data_state or 'data_retiring' not in data_state:
        raise RuntimeError('Reading-data state declarations changed')
    unit = ['#include "view_host.h"', source[start:end], data_state, '#include "view_core_bridge.h"']
    unit += [signature for _, signature, _ in functions]
    for offset, _, body in functions:
        line = source.count("\n", 0, offset) + 1
        unit += [f'#line {line} "{source_path.as_posix()}"', body]
    unit += ['#include "view_host.c"']
    (build / "view_unit.c").write_text("\n".join(unit), encoding="utf-8")

    # Keep the value types used by scheduling tied to the production headers.
    document = (root / "modules/books/document/document.h").read_text(encoding="utf-8")
    values = document[document.index("typedef enum { DOCUMENT_TEXT"):document.index("typedef struct document_source")]
    anchor = re.search(r"typedef struct\s*\{[^{}]*\}\s*document_anchor_t;", document)[0]
    values = anchor + "\n" + values
    values = re.sub(r"^((?:document_record_request_t \*|int |void )document_record_(?:request|take|cancel)\([^;]*;)",
                    r"static \1", values, flags=re.MULTILINE)
    macros = "\n".join(re.findall(
        r"^#define DOCUMENT_(?:TEXT_LIMIT|TITLE_MAX|CONVERSION_VERSION)\b[^\n]*", document, re.MULTILINE))
    reader_types = (reader / "reader_types.h").read_text(encoding="utf-8")
    reader_types = re.sub(r"^#include[^\n]*", "", reader_types, flags=re.MULTILINE)
    public = (reader / "reader.h").read_text(encoding="utf-8")
    position = public[public.index("typedef struct {\n    document_cursor_t cursor;"):public.index("typedef struct {\n    const char *app_id;")]
    (build / "view_types.h").write_text(macros + "\n" + values + reader_types + position, encoding="utf-8")
    return len(functions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--case", action="append")
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--sanitize", action="store_true", help="Enable AddressSanitizer on MSVC too.")
    parser.add_argument("--trace", action="store_true", help="Exercise the production background trace option.")
    parser.add_argument("--probe-backward-index", action="store_true",
                        help="Test a temporary backward-index ordering change; never edit production.")
    parser.add_argument("--regress-old", choices=("images", "save", "both", "glyph-single",
                        "glyph-count-unbounded", "glyph-time-unbounded", "bind-idle", "warm-gate",
                        "pause-ignored", "pause-no-idle", "paused-index-stall", "reverse-quadratic",
                        "preview-cache-required", "preview-global-first", "preview-clip-unchecked",
                        "preview-turn-link-stale", "preview-requires-chapters", "preview-metadata-only",
                        "preview-refill-starved"),
                        help="Use a temporary counterfactual or metadata-only comparison.")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    tests = Path(__file__).resolve().parent
    root = tests.parents[1]
    reader = root / "modules/common/reader"
    digest = hashlib.sha256((reader / "reader_view.c").read_bytes()).hexdigest()
    print(f"Production reader_view.c SHA256: {digest}", flush=True)
    mode = 'TEMPORARY backward-index probe' if args.probe_backward_index else (
        'TEMPORARY counterfactual' if args.regress_old else 'production')
    print(f"Mode: {mode}; "
          f"regression={args.regress_old or 'none'}; trace={args.trace}; ASan={args.sanitize}", flush=True)
    compiler, env = compiler_environment(args.cc)
    with tempfile.TemporaryDirectory(prefix="view_build_") as output:
        build = Path(output)
        count = generate(root, build, args.regress_old, args.probe_backward_index)
        executable = build / ("view_host.exe" if os.name == "nt" else "view_host")
        data = root / "components/compute_reading_data"
        sources = [build / "view_unit.c", reader / "reader_page_cache.c", reader / "compute/reader_kernel.c", reader / "compute/reader_page.c",
                   tests / "view_data_transport.c", tests.parent / "compute_reading_data/protocol.c",
                   data / "reading_data_client.c", data / "reading_data_remote.c"]
        includes = [build, tests, reader, reader / "compute/include", root / "src/ui/platform",
                    tests.parent / "compute_reading_data", data / "include", root / "src/platform", root / "components/compute/include"]
        if Path(compiler).stem.lower() == "cl":
            # C4127: compile-time trace gate; C4200: flexible arrays;
            # C4244: kernel narrowing; C4701: page_step's
            # result is read only when reader_compute_create(&result) was called.
            command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC",
                       "/wd4127", "/wd4200", "/wd4244", "/wd4701", "/D_CRT_SECURE_NO_WARNINGS",
                       *(f"/I{p}" for p in includes), *(str(p) for p in sources), f"/Fe{executable}"]
            command += ["/fsanitize=address", "/Zi"] if args.sanitize else ["/Od", "/RTC1"]
            if args.trace: command.append("/DVIEW_BACKGROUND_TRACE=1")
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       *(f"-I{p}" for p in includes), *(str(p) for p in sources), "-o", str(executable)]
            if args.trace: command.append("-DVIEW_BACKGROUND_TRACE=1")
        print(f"Compiler: {compiler}; extracted {count} production functions", flush=True)
        subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
        available = subprocess.check_output([str(executable), "--list"], cwd=build, env=env,
                                            text=True, timeout=args.timeout).splitlines()
        selected = args.case or available
        unknown = set(selected) - set(available)
        if unknown:
            parser.error(f"Unknown cases: {', '.join(sorted(unknown))}")
        failures = []
        for name in selected:
            try:
                subprocess.run([str(executable), name], cwd=build, env=env, check=True, timeout=args.timeout)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                failures.append(name)
                print(f"FAIL {name}: {error}", flush=True)
        print(f"View scheduling: {len(selected) - len(failures)}/{len(selected)} cases passed.", flush=True)
        if failures:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
