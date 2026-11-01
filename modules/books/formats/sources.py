"""Sources and include directories for the books structured converters."""
from pathlib import Path

FORMAT_SOURCES = ["formats.c", "book_fs.c", "book_stream.c", "book_xml.c", "book_markup.c", "book_svg_markup.c",
                  "book_markdown.c", "book_epub.c", "book_note.c"]
XML_SOURCES = ["SAX.c", "SAX2.c", "entities.c", "encoding.c", "error.c",
               "parserInternals.c", "parser.c", "tree.c", "hash.c", "list.c",
               "xmlIO.c", "xmlmemory.c", "uri.c", "valid.c", "globals.c",
               "threads.c", "xmlstring.c", "dict.c", "chvalid.c",
               "HTMLparser.c", "HTMLtree.c"]

def source_files(directory):
    root = Path(directory)
    return [str(root / p) for p in FORMAT_SOURCES] + [
        str(root / "../third_party/libxml2" / p) for p in XML_SOURCES
    ] + [str(root / "../third_party/miniz/miniz.c")]

def include_paths(directory):
    root = Path(directory)
    return [str(root), str(root / "../third_party/libxml2"),
            str(root / "../third_party/libxml2/include")]
