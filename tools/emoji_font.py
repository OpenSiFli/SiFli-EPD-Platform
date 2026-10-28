"""Build the static monochrome fallback used by TinyTTF (fonttools required)."""
from pathlib import Path
import hashlib

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools import subset

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "font/emoji/NotoEmoji-Variable.ttf"
TARGET = ROOT / "font/emoji/EPD-Emoji.ttf"
SOURCE_SHA256 = "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551"


def main():
    assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == SOURCE_SHA256
    font = TTFont(SOURCE, recalcTimestamp=False)
    font = instantiateVariableFont(font, {"wght": 400}, inplace=True)
    codepoints = set(font.getBestCmap())
    options = subset.Options()
    # TinyTTF consumes outlines/cmap, not OpenType substitutions. Keep every
    # encoded character, but omit unencoded ligatures and variable-font data.
    options.layout_features = []
    options.drop_tables += ["GSUB", "GPOS", "GDEF", "STAT", "vhea", "vmtx"]
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    options.name_legacy = True
    options.name_languages = [0x409]
    worker = subset.Subsetter(options=options)
    worker.populate(unicodes=codepoints)
    worker.subset(font)
    names = {1: "EPD Emoji", 2: "Regular", 3: "EPD Emoji Regular 3.002",
             4: "EPD Emoji Regular", 6: "EPDEmoji-Regular"}
    for name_id, value in names.items():
        font["name"].setName(value, name_id, 3, 1, 0x409)
    font.save(TARGET)
    result = TTFont(TARGET)
    assert set(result.getBestCmap()) == codepoints
    assert "glyf" in result and "fvar" not in result
    print(f"{TARGET.name}: {TARGET.stat().st_size} bytes, {len(codepoints)} codepoints")


if __name__ == "__main__":
    main()
