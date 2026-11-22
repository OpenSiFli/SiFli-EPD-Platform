"""Capture BODY/NAV/resume/entry behavior through the real remote and codecs."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from epub_body_nav_tools import HERE, ROOT, mirror, package
from epub_large_run import build_remote
from epub_toc_budget_run import wire_events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--epub', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    overrides = mirror(args.candidate.resolve(), output / 'source')
    exe = build_remote(output, overrides['book_epub.c'], 'body-nav', {},
                       source_overrides=overrides, host_source=HERE/'epub_body_nav_remote.c')
    results = []
    invalid=subprocess.run([str(exe),'--invalid-flags'],capture_output=True,text=True,timeout=30)
    (output/'invalid-flags.log').write_text(invalid.stdout+invalid.stderr,encoding='utf-8')
    assert not invalid.returncode,invalid.stdout+invalid.stderr
    results.append(dict(case='invalid-flags',passed=True))
    io=subprocess.run([str(exe),'--io-failure',str(args.epub.resolve())],capture_output=True,text=True,timeout=30)
    (output/'io-failure.log').write_text(io.stdout+io.stderr,encoding='utf-8')
    assert not io.returncode,io.stdout+io.stderr
    results.append(dict(case='io-failure',passed=True))
    cases = [('normal', 3, 7, 0, 0, 0), ('body', 35, 7, 0, 0, 0),
             ('first', 163, 4294967295, 0, 0, 0), ('restore', 35, 7, 4096, 0, 0),
             ('nav', 65, 7, 0, 0, 0), ('nav-resume', 65, 7, 0, 25, 0),
             ('nav-invalid-resume', 65, 7, 0, 100000, -1)]
    for name, flags, spine, anchor, skip, expected in cases:
        wire = output/(name+'.wire')
        p = subprocess.run([str(exe), str(args.epub.resolve()), str(flags), str(spine), str(anchor),
                            str(skip), str(wire), str(expected)], capture_output=True, text=True, timeout=180)
        (output/(name+'.log')).write_text(p.stdout+p.stderr, encoding='utf-8')
        print(name, p.stdout+p.stderr, flush=True)
        results.append(dict(case=name, passed=p.returncode == 0))
        assert not p.returncode
    def events(name): return list(wire_events((output/(name+'.wire')).read_bytes()))
    normal, body, first, nav, resume = map(events, ('normal','body','first','nav','nav-resume'))
    nav_kind = lambda h: h[0] == 9 and not h[4] or h[0] == 5 and h[2] == 1
    target_kind = lambda h: h[0] == 10 and h[5]&0x200
    assert b''.join(r for h,r in normal if not nav_kind(h) and not target_kind(h)) == b''.join(
        r for h,r in body if not target_kind(h))
    nav_records = [r for h,r in nav if nav_kind(h)]
    assert b''.join(nav_records[25:]) == b''.join(r for h,r in resume if nav_kind(h))
    assert b''.join(r for h,r in normal if nav_kind(h)) == b''.join(nav_records)
    chosen = [h[2] for h,r in first if h[0] == 9 and h[4]]
    assert chosen == [7]
    target_at = next(i for i,(h,r) in enumerate(first) if h[0] == 10 and h[5]&0x200)
    assert any(h[0] == 1 for h,r in first[target_at:])
    assert not any(h[0] == 6 for h,r in first[target_at:target_at+3])
    package(args.candidate.resolve().parent)
    (output/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('PASS body bytes, full navigation, exact resumed prefix, automatic first chapter and text target')


if __name__ == '__main__': main()
