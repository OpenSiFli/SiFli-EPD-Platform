#!/usr/bin/env python3
"""Generate the platform's bounded single-character and phrase pinyin tables."""

import argparse
import base64
from collections import defaultdict
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import re
import urllib.request


REPO = Path(__file__).resolve().parents[1]
ASSETS = REPO / "assets/pinyin"
COMMIT = "49aebad1c1cfbbcaa9288ffed5161e79e57c3679"
BASE = ("https://android.googlesource.com/platform/packages/inputmethods/"
        f"PinyinIME/+/{COMMIT}/")
RAW_PATH = "jni/data/rawdict_utf16_65105_freq.txt"
RAW_SHA256 = "408700f28a56091fa07f3b849a0f134fbfc71e6b2ae9b3f52973a5b076f599ff"
NOTICE_SHA256 = "b5830d96fb5a7e7e7ebcc295f352846b4b998e78fdc8f9aa68e134d2e4b39986"
SINGLE_SHA256 = "5eace7172a86f359019be32cad0913091ebd49cc583df4280cc145b3c1559c3d"
PHRASE_SHA256 = "dcfdf74a95da83f365cff346e47bc9efd6c7273e2d2487e72357a497647bd51d"
PHRASE_LIMIT = 2800
PHRASE_KEY_MAX = 24
TABLE_BUDGET = 94000
SINGLE_SOURCE = ASSETS / "aosp_single_characters.tsv"
PHRASE_SOURCE = ASSETS / "aosp_phrases.tsv"
LICENSE = ASSETS / "LICENSE"
OUTPUT = REPO / "src/ui/platform/epd_pinyin_data.inc"


def verify(data, expected, name):
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"SHA-256 mismatch: {name}")
    return data


def download(path, expected):
    with urllib.request.urlopen(BASE + path + "?format=TEXT", timeout=45) as response:
        data = base64.b64decode(response.read(), validate=True)
    return verify(data, expected, path)


def source_rows(data):
    """Read word, positive frequency, GBK flag, then one spelling per character."""
    singles, phrases = [], {}
    for line_number, line in enumerate(data.decode("utf-16").splitlines(), 1):
        fields = line.split()
        if not fields:
            continue
        word, frequency, gbk_flag, *syllables = fields
        if len(fields) != 3 + len(word) or gbk_flag not in ("0", "1"):
            raise ValueError(f"Invalid record on line {line_number}")
        score = Decimal(frequency)
        if not score.is_finite() or score <= 0:
            raise ValueError(f"Invalid frequency on line {line_number}")
        if any(not re.fullmatch(r"[a-z]{1,6}", s) for s in syllables):
            raise ValueError(f"Unsupported spelling on line {line_number}")
        if gbk_flag != "0":
            continue
        if any(not 0x4E00 <= ord(c) <= 0x9FFF for c in word):
            continue
        if any(len(c.encode("gb2312")) != 2 for c in word):
            raise ValueError(f"Invalid GB2312 flag on line {line_number}")
        row = ("'".join(syllables), word, frequency, str(line_number))
        if len(word) == 1:
            singles.append(row)
        elif 2 <= len(word) <= 4:
            identity = row[:2]
            if identity not in phrases or score > Decimal(phrases[identity][2]):
                phrases[identity] = row
    ranked = sorted(phrases.values(), key=lambda r: (-Decimal(r[2]), r[0].replace("'", ""), r[1], r[0]))
    return singles, ranked[:PHRASE_LIMIT]


def render_source(rows, phrases=False):
    modification = (f"retain the {PHRASE_LIMIT} highest-frequency 2-4 character GB2312 phrases."
                    if phrases else "retain single GB2312 CJK characters (source GBK flag 0).")
    heading = ("# AOSP PinyinIME; Copyright (c) 2009, The Android Open Source Project\n"
               "# SPDX-License-Identifier: Apache-2.0\n"
               f"# Source commit: {COMMIT}\n"
               f"# Source file: {RAW_PATH}\n"
               f"# Original file SHA-256: {RAW_SHA256}\n"
               f"# Modified: {modification}\n"
               + ("# pinyin\tphrase\tfrequency\toriginal_line\n" if phrases
                  else "# pinyin\tcharacter\tfrequency\toriginal_line\n"))
    return (heading + "".join("\t".join(row) + "\n" for row in rows)).encode("utf-8")


def read_rows(path, phrases=False):
    data = path.read_bytes()
    verify(data, PHRASE_SHA256 if phrases else SINGLE_SHA256, path.name)
    rows = []
    for line in data.decode("utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        row = tuple(line.split("\t"))
        if len(row) != 4:
            raise ValueError(f"Invalid source row in {path}")
        pinyin, word, frequency, original_line = row
        syllables = pinyin.split("'")
        if (len(syllables) != len(word) or
                any(not re.fullmatch(r"[a-z]{1,6}", s) for s in syllables) or
                any(not 0x4E00 <= ord(c) <= 0x9FFF for c in word) or
                any(len(c.encode("gb2312")) != 2 for c in word) or
                not Decimal(frequency).is_finite() or Decimal(frequency) <= 0 or
                not original_line.isdigit() or int(original_line) < 1 or
                not (2 <= len(word) <= 4 if phrases else len(word) == 1)):
            raise ValueError(f"Invalid candidate in {path}: {row}")
        rows.append(row)
    if phrases and len(rows) != PHRASE_LIMIT:
        raise ValueError("Unexpected phrase count")
    return rows


def dictionary(singles, phrases):
    groups = defaultdict(dict)
    for key, word, frequency, _line in singles:
        groups[key][word] = max(Decimal(frequency), groups[key].get(word, Decimal(0)))
    characters = ""
    syllables = []
    for key, values in sorted(groups.items()):
        words = sorted(values, key=lambda word: (-values[word], word))
        syllables.append((key, len(characters.encode("utf-8")), len(words)))
        characters += "".join(words)
    keys, texts, entries = bytearray(), bytearray(), []
    key_offsets = {}
    seen = set()
    for spelling, word, frequency, _line in sorted(
            phrases, key=lambda r: (r[0].replace("'", ""), -Decimal(r[2]), r[1], r[0])):
        key = spelling.replace("'", "")
        if (spelling, word) in seen:
            raise ValueError("Duplicate phrase")
        seen.add((spelling, word))
        if len(key) > PHRASE_KEY_MAX or len(word.encode("utf-8")) >= 40:
            raise ValueError("Phrase exceeds engine capacity")
        if key not in key_offsets:
            key_offsets[key] = len(keys)
            keys.extend(key.encode("ascii") + b"\0")
        boundary, length = 0, 0
        for syllable in spelling.split("'"):
            length += len(syllable)
            boundary |= 1 << length
            if syllable not in groups:
                raise ValueError(f"Unknown syllable: {syllable}")
        entries.append((key_offsets[key], len(texts), boundary))
        texts.extend(word.encode("utf-8") + b"\0")
    character_bytes = characters.encode("utf-8")
    if max(len(character_bytes), len(keys), len(texts)) > 65535:
        raise ValueError("Dictionary exceeds 16-bit pool offsets")
    sizes = {"single_index": len(syllables) * 12, "single_text": len(character_bytes) + 1,
             "phrase_index": len(entries) * 8, "phrase_keys": len(keys) + 1,
             "phrase_text": len(texts) + 1}
    sizes["total"] = sum(sizes.values())
    if sizes["total"] > TABLE_BUDGET:
        raise ValueError(f"Dictionary exceeds {TABLE_BUDGET} byte table budget")
    return syllables, character_bytes, bytes(keys), bytes(texts), entries, sizes


def c_pool(name, data):
    # Octal escapes make generated UTF-8 independent of compiler source charset.
    lines = [f"static const char {name}[] ="]
    for start in range(0, len(data), 48):
        part = data[start:start + 48]
        lines.append('    "' + "".join(chr(b) if 97 <= b <= 122 else f"\\{b:03o}" for b in part) + '"')
    return lines + [";", ""]


def render_c(tables):
    syllables, characters, keys, texts, entries, _sizes = tables
    lines = ["/*", " * Generated by tools/generate_pinyin.py.",
             " * Derived from AOSP PinyinIME; Copyright (c) 2009, The Android Open Source Project.",
             " * SPDX-License-Identifier: Apache-2.0", f" * Source commit: {COMMIT}",
             " * Modified: GB2312 single characters and bounded high-frequency phrases;",
             " * frequency ranking and compact platform dictionary layout.",
             " * Original notice: assets/pinyin/LICENSE.", " */", "",
             "typedef struct", "{", "    char key[7];", "    uint16_t offset;",
             "    uint16_t count;", "} epd_pinyin_syllable_t;", "",
             "typedef struct", "{", "    uint16_t key;", "    uint16_t text;",
             "    uint32_t boundaries;", "} epd_pinyin_phrase_t;", "",
             f"#define EPD_PINYIN_SYLLABLE_COUNT {len(syllables)}u",
             f"#define EPD_PINYIN_PHRASE_COUNT {len(entries)}u",
             f"#define EPD_PINYIN_PHRASE_KEY_MAX {PHRASE_KEY_MAX}u", "",
             "static const epd_pinyin_syllable_t epd_pinyin_syllables[] = {"]
    lines += [f'    {{ "{key}", {offset}, {count} }},' for key, offset, count in syllables]
    lines += ["};", ""] + c_pool("epd_pinyin_characters", characters)
    lines += c_pool("epd_pinyin_phrase_keys", keys)
    lines += c_pool("epd_pinyin_phrase_text", texts)
    lines += ["static const epd_pinyin_phrase_t epd_pinyin_phrases[] = {"]
    lines += [f"    {{ {key}, {text}, 0x{boundary:08x}u }}," for key, text, boundary in entries]
    lines += ["};", ""]
    return "\n".join(lines).encode("ascii")


def render_metadata(tables, generated, singles, phrases):
    syllables, characters, _keys, _texts, _entries, sizes = tables
    metadata = {
        "library": "AOSP PinyinIME",
        "repository": "https://android.googlesource.com/platform/packages/inputmethods/PinyinIME/",
        "revision": COMMIT, "source": RAW_PATH, "source_url": BASE + RAW_PATH,
        "source_sha256": RAW_SHA256, "source_encoding": "UTF-16LE with BOM",
        "source_record_format": "word frequency gbk_flag syllable_1 ... syllable_N",
        "license": "Apache-2.0", "license_file": "LICENSE",
        "license_source_url": BASE + "NOTICE", "license_sha256": NOTICE_SHA256,
        "repository_generator": "tools/generate_pinyin.py",
        "source_subsets": {p.relative_to(REPO).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in (SINGLE_SOURCE, PHRASE_SOURCE)},
        "generated_source": OUTPUT.relative_to(REPO).as_posix(),
        "generated_source_sha256": hashlib.sha256(generated).hexdigest(),
        "syllables": len(syllables), "characters": len(set(characters.decode("utf-8"))),
        "readings": len(characters) // 3, "phrases": len(phrases),
        "phrase_characters_min": 2, "phrase_characters_max": 4,
        "minimum_phrase_frequency": str(min(Decimal(r[2]) for r in phrases)),
        "table_bytes": sizes, "table_budget_bytes": TABLE_BUDGET,
        "processing": [
            "Validate the pinned upstream digest before deriving offline source subsets.",
            "Retain all original single GB2312 CJK characters and polyphonic readings.",
            "Deduplicate readings using maximum source frequency.",
            f"Keep the top {PHRASE_LIMIT} two-to-four-character phrases by source frequency.",
            "Sort keys by ASCII, rank candidates by descending frequency then Unicode text.",
            "Preserve source syllable boundaries for apostrophe-constrained lookup.",
            "Store UTF-8 pools, 16-bit offsets and 32-bit phrase boundary masks.",
            "Table sizes use 12-byte single indexes and 8-byte phrase indexes; excludes engine text.",
        ],
    }
    return (json.dumps(metadata, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def write_or_check(path, data, check):
    if check:
        if not path.is_file() or path.read_bytes() != data:
            raise SystemExit(f"Generated file is not up to date: {path.relative_to(REPO)}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="refresh pinned source subsets and notice")
    parser.add_argument("--upstream", type=Path, help="use a local copy of the pinned raw source")
    parser.add_argument("--check", action="store_true", help="verify output without writing")
    args = parser.parse_args()
    if args.fetch or args.upstream:
        data = (verify(args.upstream.read_bytes(), RAW_SHA256, args.upstream)
                if args.upstream else download(RAW_PATH, RAW_SHA256))
        singles, phrases = source_rows(data)
        write_or_check(SINGLE_SOURCE, render_source(singles), args.check)
        write_or_check(PHRASE_SOURCE, render_source(phrases, True), args.check)
        if args.fetch:
            write_or_check(LICENSE, download("NOTICE", NOTICE_SHA256), args.check)
    verify(LICENSE.read_bytes(), NOTICE_SHA256, LICENSE)
    singles, phrases = read_rows(SINGLE_SOURCE), read_rows(PHRASE_SOURCE, True)
    tables = dictionary(singles, phrases)
    generated = render_c(tables)
    write_or_check(OUTPUT, generated, args.check)
    write_or_check(ASSETS / "source.json", render_metadata(tables, generated, singles, phrases), args.check)
    print(f"{len(tables[0])} syllables; {len(tables[1]) // 3} single readings; {len(phrases)} phrases")
    print(json.dumps(tables[-1], sort_keys=True))


if __name__ == "__main__":
    main()
