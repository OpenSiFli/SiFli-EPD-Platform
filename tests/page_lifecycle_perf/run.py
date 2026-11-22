"""Compile intact production page functions with deterministic UI/service doubles.

Run: python tests/page_lifecycle_perf/run.py [--cc COMPILER]
The directory/cursor engine is covered separately by storage_perf/file_page_run.py.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment


def function(source, name):
    match = re.search(r"^(?:static )?[\w *]+\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise ValueError(name)
    start = match.start()
    depth = 1
    # Skip strings and comments while counting braces, including compound literals.
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*|[{}]',
                         source[match.end():], re.S)
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:match.end() + token.end()]
    raise ValueError("Unclosed function " + name)


FUNCTIONS = {
    "app_settings": "is_settings_list settings_page list_selected settings_list_create settings_rows timezone_text timezone_rows option_rows frontlight_mode_name settings_resume settings_restore_focus settings_stop",
    "app_portal": "place_label render portal_page_create portal_page_resume portal_page_process portal_page_stop",
    "app_memory_page": "memory_layout memory_render memory_page_restore_focus memory_page_process memory_page_resume memory_page_stop memory_selected memory_page_create",
    "app_files": "files_title_font files_body_font files_value_font files_title_x files_title_y volume_selected files_restore_focus files_render files_retranslate files_resume storage_page_resume files_create storage_page_create",
    "app_management": "app_source_is_package app_source_path app_sources_free app_source_name_compare app_source_compare app_source_refresh app_list_layout app_list_selected list_refresh app_list_create app_list_retranslate app_list_resume app_list_stop app_list_restore_focus app_list_process",
}


def generate(output):
    sources = {name: (ROOT / f"src/ui/apps/{name}.c").read_text(encoding="utf-8") for name in FUNCTIONS}
    type_ends = {"app_settings": "static void device_name_layout", "app_portal": "static int32_t place_label",
                 "app_memory_page": "static bool memory_layout", "app_files": "static bool files_private_entry",
                 "app_management": "static bool app_source_is_package"}
    types = []
    definitions = []
    for name, names in FUNCTIONS.items():
        source = sources[name]
        start = source.index("enum {") if name == "app_portal" else source.index("#define")
        types.append(source[start:source.index(type_ends[name])])
        definitions.extend(function(source, f) for f in names.split())
    settings = sources["app_settings"]
    types.append(settings[settings.index("#define LINK("):settings.index("static void settings_rows(")])
    # Include the real public create wrappers, so direct callers use the same contract.
    wrappers = re.findall(r"^void \w+_create\(lv_obj_t \*screen\) \{ settings_list_create\(screen\); \}", settings, re.M)
    definitions.extend(wrappers)
    (output / "types.inc").write_text("\n".join(types), encoding="utf-8")
    (output / "pages.inc").write_text("\n\n".join(definitions), encoding="utf-8")
    (output / "prototypes.inc").write_text("\n".join(f[:f.index("{")].strip() + ";" for f in definitions), encoding="utf-8")
    pages = (ROOT / "src/ui/apps/ui_pages.c").read_text(encoding="utf-8")
    creates = {re.search(r"\b(\w+)\(", f).group(1) for f in definitions if re.search(r"\b\w+_create\(", f.split("{")[0])}
    registrations = re.findall(r"\[(UI_PAGE_\w+)\] = \{(\w+), (\w+),", pages)
    registrations = [r for r in registrations if r[1] in creates]
    assert len(registrations) == 25, registrations
    (output / "registrations.inc").write_text("\n".join('{%s, %s, %s},' % r for r in registrations))
    ids = sorted(set(re.findall(r"\bUI_PAGE_\w+", "\n".join(sources.values()) + pages)))
    settings_header = (ROOT / "src/ui/ui_settings.h").read_text(encoding="utf-8")
    settings_enum = re.search(r"typedef enum\s*\{.*?\}\s*ui_setting_id_t;", settings_header, re.S).group()
    (output / "ids.inc").write_text("enum {" + ",".join(ids) + "};\n" + settings_enum)
    # Audit production references, including address-taking through operation tables.
    allowed = {ROOT / "src/ui/apps/ui_pages.c", ROOT / "src/ui/ui_internal.h"}
    allowed.update(ROOT / f"src/ui/apps/{name}.c" for name in FUNCTIONS)
    pattern = re.compile(r"\b(?:" + "|".join(creates - {"settings_list_create"}) + r")\b")
    for directory in (ROOT / "src", ROOT / "modules"):
        for path in directory.rglob("*"):
            if path.suffix in (".c", ".h") and path not in allowed:
                assert not pattern.search(path.read_text(encoding="utf-8", errors="replace")), f"Additional create caller: {path}"
    print(f"Production registrations/caller audit: {len(registrations)} pages", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    args = parser.parse_args()
    compiler, environment = compiler_environment(args.cc)
    with tempfile.TemporaryDirectory(prefix="page-lifecycle-") as directory:
        output = Path(directory)
        generate(output)
        executable = output / "page_lifecycle.exe"
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W3", "/Od", "/RTC1", "/D_CRT_SECURE_NO_WARNINGS",
                       f"/I{output}", f"/I{HERE}", str(HERE / "host.c"), f"/Fe:{executable}"]
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra", f"-I{output}", f"-I{HERE}", str(HERE / "host.c"), "-o", str(executable)]
        subprocess.run(command, cwd=output, env=environment, check=True, timeout=60)
        subprocess.run([str(executable)], cwd=output, check=True, timeout=30)


if __name__ == "__main__":
    main()
