"""Independent callback converter source list; no build-system mutation."""
from pathlib import Path
import runpy


def configuration(directory=None):
    here = Path(directory or Path(__file__).resolve().parent).resolve()
    formats = here.parent / "formats"
    existing = runpy.run_path(str(formats / "sources.py"))
    xml = here.parent / "third_party/libxml2"
    xml_sources = [str(xml / name) for name in existing["XML_SOURCES"]]
    return {
        "sources": [str(here / "book_compute.c"), str(here / "book_timing.c"), str(here / "book_fs_callbacks.c"),
                    str(here / "book_compute_txt.c"), str(here / "book_compute_slice.c"),
                    str(here / "vendor_markup.c"), str(here / "book_compute_markup.c"),
                    str(here / "book_compute_mobi.c"), str(here / "mobi_port.c")]
        + [str(here / ("mobi_" + name + ".c")) for name in
           ("buffer", "compression", "index", "memory", "meta", "parse_rawml", "read", "structure", "util")]
        + [str(formats / name) for name in existing["FORMAT_SOURCES"] if name not in ("book_fs.c", "book_svg_markup.c")]
        + xml_sources + [str(here.parent / "third_party/miniz/miniz.c")],
        "includes": [str(here), str(here / "mobi_include"),
                     str(here.parent / "codecs/mobi"), str(here.parent / "third_party/libmobi/src"),
                     *existing["include_paths"](formats)],
        "defines": ["BOOK_FORMAT_ACPU", "LIBXML_STATIC"],
        "force_includes": {source: str(here / "book_compute_xml_port.h") for source in xml_sources},
    }


if __name__ == "__main__":
    import json
    print(json.dumps(configuration(), indent=2))
