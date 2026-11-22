"""Extract unchanged reader functions for the document/layout host regression."""
import hashlib
import re
from pathlib import Path


def function(source, name):
    match = re.search(r"^(?:static )?[^\n;]+\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"Missing production function: {name}")
    opening = source.index("{", match.start())
    depth = 0
    tokens = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for token in re.finditer(tokens, source[opening:], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[match.start():opening + token.end()]
    raise ValueError(f"Unclosed production function: {name}")


def extract(path: Path, build: Path):
    source = path.read_text(encoding="utf-8")
    page = re.search(r"typedef struct \{\s*EPD_READER_PAGE_FIELDS\(document_cursor_t\);.*?\} view_page_t;", source, re.S)
    if not page:
        raise ValueError("Missing production view_page_t")
    has_target = bool(re.search(r"static bool page_target\(", source))
    (build / "reader_page_type.inc").write_text(page.group() + "\n", encoding="utf-8")
    names = ["cursor_equal", "font_slot", "font_for", "width_available", "compute_cancel",
             "page_reset", "utf8_letter"]
    if re.search(r"static int page_error\(", source):
        names.append("page_error")
    names.append("decode_at")
    if has_target:
        names.append("page_target")
    names += ["page_step", "page_runnable"]
    output = f"#define HCPU_HAS_PAGE_TARGET {int(has_target)}\n"
    metrics = re.search(r'^#define VIEW_METRICS_WINDOW\s+(\S+)', source, re.M)
    if metrics:
        output += '#ifndef VIEW_METRICS_WINDOW\n#define VIEW_METRICS_WINDOW ' + metrics[1] + '\n#endif\n'
    output += "\n\n".join(function(source, name) for name in names) + "\n"
    if re.search(r'^static void document_flow\(', source, re.M):
        output += '#define HCPU_HAS_DOCUMENT_FLOW 1\n' + function(source, 'document_flow') + '\n'
    else:
        output += '#define HCPU_HAS_DOCUMENT_FLOW 0\n'
    (build / "reader_page_functions.inc").write_text(output, encoding="utf-8")
    print(f"Reader extraction: {path} sha256={hashlib.sha256(path.read_bytes()).hexdigest()}", flush=True)
