"""Compare original and callback converter events using the real parser sources.

Run with Python 3 and GCC/Clang on Linux (including WSL). All generated files
stay in a temporary directory under this test directory. --epub PATH adds an
existing EPUB to the byte-for-byte comparison corpus.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import zipfile
import struct
import zlib
import io

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
COMPUTE = ROOT / "modules/books/compute"


def fixtures(build, extra, svg=True):
    folder = build / "fixtures"
    folder.mkdir()
    long_text = "正文🙂中文 UTF-8 " * 2000
    chapter = ('<html xmlns="http://www.w3.org/1999/xhtml"><head>'
               '<link rel="stylesheet" href="style.css"/></head><body>'
               '<h1 id="start">Chapter One</h1><p><b>Bold</b> and <i>italic</i> '
               '<a href="two.xhtml#end">next</a></p><p>' + long_text + '</p>'
               '<img src="image.png"/><img src="art.svg"/>'
               '<svg xmlns="http://www.w3.org/2000/svg" width="5" height="3"><rect width="5" height="3" fill="black"/></svg></body></html>')
    container = ('<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                 '<rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>')
    opf = ('<package xmlns="http://www.idpf.org/2007/opf" version="3.0">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           '<dc:title>测试 Book</dc:title><dc:creator>Author</dc:creator></metadata>'
           '<manifest><item id="one" href="one.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="two" href="two.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
           '</manifest><spine><itemref idref="one"/><itemref idref="two"/></spine></package>')
    data = {"mimetype": "application/epub+zip", "META-INF/container.xml": container,
            "OPS/book.opf": opf, "OPS/one.xhtml": chapter,
            "OPS/two.xhtml": '<html xmlns="http://www.w3.org/1999/xhtml"><body><h2 id="end">End</h2></body></html>',
            "OPS/nav.xhtml": '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><a href="one.xhtml#start">First</a></li><li><a href="two.xhtml#end">Second</a></li></ol></nav></body></html>',
            "OPS/style.css": 'p { text-align: center; } b { font-weight: bold; }',
            "OPS/image.png": png(),
            "OPS/art.svg": '<svg xmlns="http://www.w3.org/2000/svg" width="5" height="3"><image href="image.png" width="5" height="3"/></svg>'}

    def archive(path, entries, compression=zipfile.ZIP_DEFLATED):
        with zipfile.ZipFile(path, "w", compression=compression) as z:
            for name, content in entries.items():
                z.writestr(name, content)

    archive(folder / "svg.epub", data)
    if not svg:
        chapter = chapter.replace('<img src="art.svg"/>', '')
        chapter = chapter[:chapter.index('<svg ')] + '</body></html>'
        data["OPS/one.xhtml"] = chapter
    archive(folder / "deflated.epub", data)
    archive(folder / "stored.epub", data, zipfile.ZIP_STORED)
    note_open = ('<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><body>'
                 '<p><a class="footnoteRef" href="#fn"><img src="image.png" data-pad="a > b')
    # Split the anchor's closing tag across two 2048-byte XML input chunks.
    note_chapter = note_open.ljust(2044) + '"></a>ordinary text</p><p id="fn">note</p></body></html>'
    archive(folder / "footnote-void.epub", {**data, "OPS/one.xhtml": note_chapter})
    split = folder / "split"
    split.mkdir()
    archive(split / "basepackage.zip", {k: v for k, v in data.items() if k not in ("OPS/one.xhtml", "OPS/two.xhtml")})
    archive(split / "chapter_10.zip", {"OPS/two.xhtml": data["OPS/two.xhtml"]})
    archive(split / "chapter_2.zip", {"OPS/one.xhtml": chapter})
    (folder / "book.md").write_text('# 标题\n**Bold** *italic* [link](#标题)\n' + long_text +
                                   '\n```\ncode\n```\n![pic](image.png)\n', encoding="utf-8")
    (folder / "image.png").write_bytes(data["OPS/image.png"])
    (folder / "bytes.bin").write_bytes(b"0123456789")
    (folder / "book.mobi").write_bytes(mobi())
    (folder / "compressed.mobi").write_bytes(mobi(compressed=True))
    markup = '<html><body><h1>Standalone HTML</h1><p>' + long_text + '</p><img src="image.png"/></body></html>'
    (folder / "chapter.html").write_text(markup, encoding="utf-8")
    (folder / "chapter.data").write_text(markup, encoding="utf-8")
    (folder / "chapter.xhtml").write_text(markup.replace('<html>', '<html xmlns="http://www.w3.org/1999/xhtml">'), encoding="utf-8")
    (folder / "invalid.epub").write_bytes(b"not a zip" * 40)
    bad = bytearray((folder / "stored.epub").read_bytes())
    bad[bad.index(b"\x89PNG")] ^= 1
    (folder / "crc.epub").write_bytes(bad)
    cases = ["deflated.epub", "stored.epub", "split", "book.md", "invalid.epub", "crc.epub", "footnote-void.epub"]
    for i, path in enumerate(extra):
        name = f"existing-{i}.epub"
        shutil.copyfile(path, folder / name)
        cases.append(name)
    txt = "第一行🙂\n第二行\n\n" + long_text + "\n"
    for encoding in ("utf-8-sig", "utf-16", "utf-16-be", "gbk"):
        content = txt.replace("🙂", "") if encoding == "gbk" else txt
        encoded = content.replace("\n", "\r\n").encode(encoding)
        if encoding == "utf-16-be": encoded = b"\xfe\xff" + encoded
        name = "text-" + encoding + ".txt"
        (folder / name).write_bytes(encoded)
        (folder / (name + ".expected")).write_bytes(content.encode("utf-8"))
    (folder / "malformed.txt").write_bytes(b"\xef\xbb\xbfabc\xed\xa0\x80\x00\xe2Z\r\n")
    (folder / "malformed.txt.expected").write_bytes("abc���Z\n".encode())
    paths = sorted(folder.rglob("*"), reverse=True)
    (folder / "index.txt").write_text("".join(
        f"{'D' if p.is_dir() else 'F'} {p.relative_to(build).as_posix()}\n" for p in paths))
    return ["fixtures/" + name for name in cases]


def png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 5, 3, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\0\0\x40\x80\xc0\xff" * 3)) + chunk(b"IEND", b""))

def mobi(compressed=False):
    text = ('<html><body><h1>MOBI standalone</h1><p>' + 'MOBI正文 ' * 6000 + '</p></body></html>').encode()
    records = [text[i:i+4096] for i in range(0, len(text), 4096)]
    header = struct.pack('>HHIHHHH', 2 if compressed else 1, 0, len(text), len(records), 4096, 0, 0)
    header += b'MOBI' + struct.pack('>IIIII', 24, 2, 65001, 1, 1)
    if compressed:
        records = [b''.join(bytes([len(r[i:i+8])]) + r[i:i+8] for i in range(0, len(r), 8)) for r in records]
    records.insert(0, header)
    pdb = bytearray(78)
    pdb[:5] = b'Tests'; pdb[60:68] = b'BOOKMOBI'
    struct.pack_into('>H', pdb, 76, len(records))
    offset = 78 + len(records) * 8 + 2
    table = bytearray()
    for i, record in enumerate(records):
        table += struct.pack('>II', offset, i * 2)
        offset += len(record)
    return pdb + table + b'\0\0' + b''.join(records)


def text_events(data):
    stream = io.BytesIO(data)
    out = bytearray()
    def field():
        length = bytearray()
        while (c := stream.read(1)) != b":":
            assert c
            length += c
        payload = stream.read(int(length))
        assert stream.read(1) == b"\n"
        return payload
    while stream.tell() < len(data):
        if data[stream.tell():].startswith(b"result:"):
            assert stream.readline() == b"result:0\n"
            break
        kind = field()
        if kind == b"text":
            assert stream.readline() == b"0\n"
            out += field()
        elif kind == b"anchor":
            assert field().startswith(b"txt-byte-")
            assert field() == b"<null>"
            assert stream.readline() == b"0\n"
        else:
            assert kind == b"paragraph"
            out += b"\n"
    return bytes(out)


def build_variant(build, cc, compute, sanitize):
    config = runpy.run_path(str(COMPUTE / "sources.py"))["configuration"]()
    destination = build / ("compute" if compute else "original")
    destination.mkdir()
    sources = list(config["sources"])
    if not compute:
        sources = [s for s in sources if Path(s).parent != COMPUTE]
        sources.append(str(ROOT / "modules/books/formats/book_fs.c"))
        sources.append(str(ROOT / "modules/books/formats/book_svg_markup.c"))
    sources.append(str(HERE / "host_test.c"))
    flags = ["-std=c11", "-D_DEFAULT_SOURCE", "-DLIBXML_STATIC", "-g", "-O1"]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie"]
    if compute:
        flags.append("-DBOOK_FORMAT_ACPU")
    flags += ["-I" + p for p in config["includes"]]

    def compile_one(source):
        obj = destination / (Path(source).stem + ".o")
        args = [cc, *flags]
        if "third_party" not in source and not Path(source).name.startswith("mobi_"):
            args += ["-Wall", "-Wextra", "-Werror"]
        if compute and source in config["force_includes"]:
            args += ["-include", config["force_includes"][source]]
        result = subprocess.run([*args, "-c", source, "-o", str(obj)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        objects = list(pool.map(compile_one, sources))
    exe = destination / "test"
    link = [cc, *map(str, objects), "-lm", "-o", str(exe)]
    if sanitize: link += ["-fsanitize=address,undefined", "-no-pie"]
    subprocess.run(link, check=True)
    if compute:
        undefined = subprocess.check_output(["nm", "-u", *map(str, objects[:-1])], text=True)
        forbidden = {"open", "open64", "read", "close", "lseek", "stat", "stat64", "fopen", "fread", "fseek", "fclose", "opendir", "readdir", "closedir"}
        imported = {line.split()[-1] for line in undefined.splitlines() if " U " in line}
        assert not (forbidden & imported), forbidden & imported
        print("ACPU object audit: no native filesystem imports", flush=True)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--epub", type=Path, action="append", default=[])
    parser.add_argument("--no-sanitize", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="build-", dir=HERE) as temporary:
        build = Path(temporary)
        cases = fixtures(build, args.epub)
        info = build / "info"
        subprocess.run([args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "modules/books/formats"),
                        str(ROOT / "modules/books/formats/formats_info.c"),
                        str(HERE / "info_test.c"), "-o", str(info)], check=True)
        subprocess.run([str(info)], cwd=build, check=True)
        print("Lightweight format detection/name linked without converter or XML", flush=True)
        original = build_variant(build, args.cc, False, not args.no_sanitize)
        compute = build_variant(build, args.cc, True, not args.no_sanitize)
        for case in cases:
            for exe, output in ((original, "original.events"), (compute, "compute.events")):
                subprocess.run([str(exe), case, output], cwd=build, check=True, timeout=60)
            a, b = (build / "original.events").read_bytes(), (build / "compute.events").read_bytes()
            assert a == b, f"Event mismatch: {case}"
            expected = b"result:0\n" if case not in ("fixtures/invalid.epub", "fixtures/crc.epub") else b"result:-1\n"
            if not case.startswith("fixtures/existing-"): assert a.endswith(expected), (case, a[-40:])
            if case == "fixtures/footnote-void.epub":
                assert b"4:link\n16:OPS/one.xhtml#fn\n0\n4:text\n0\n13:ordinary text\n" in a
            print(f"identical events: {case} ({len(a)} bytes)", flush=True)
        for case in ("fixtures/deflated.epub", "fixtures/book.md", "fixtures/split"):
            subprocess.run([str(compute), "--selftest", case], cwd=build, check=True, timeout=120)
        for expected in sorted((build / "fixtures").glob("*.txt.expected")):
            case = str(expected.relative_to(build)).removesuffix(".expected")
            subprocess.run([str(compute), case, "txt.events"], cwd=build, check=True, timeout=60)
            assert text_events((build / "txt.events").read_bytes()) == expected.read_bytes(), case
            print("normalized UTF-8 matches: " + case, flush=True)
        subprocess.run([str(compute), "--selftest", "fixtures/text-utf-8-sig.txt"], cwd=build, check=True, timeout=120)
        for case in ("fixtures/book.mobi", "fixtures/compressed.mobi", "fixtures/chapter.html", "fixtures/chapter.xhtml"):
            subprocess.run([str(compute), case, "extra.events"], cwd=build, check=True, timeout=60)
            data = (build / "extra.events").read_bytes()
            assert data.endswith(b"result:0\n"), (case, data[-100:])
            subprocess.run([str(compute), "--selftest", case], cwd=build, check=True, timeout=180)
        print("All callback converter tests passed", flush=True)


if __name__ == "__main__":
    main()
