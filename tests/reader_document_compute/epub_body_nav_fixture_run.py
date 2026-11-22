"""Capture navigation interruption fixtures through an existing real remote binary."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True
from epub_large_run import fixture
from epub_toc_budget_run import wire_events


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    seed=out/'seed.epub';fixture(seed,65)
    with zipfile.ZipFile(seed) as z: entries={n:z.read(n) for n in z.namelist()}
    entries['OPS/toc.ncx']=entries['OPS/toc.ncx'].replace(b'Chapter 0',b'Cover')
    entries['OPS/c0.xhtml']=b'<html xmlns="http://www.w3.org/1999/xhtml"><body><img src="image.png"/></body></html>'
    entries['OPS/c1.xhtml']=('''<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body>
<img src="image.png"/><h1 id="title">Chapter 1</h1><p>Opening prose paragraph.
<a epub:type="noteref" href="note.xhtml#n1">note</a><a href="c64.xhtml#title">last chapter</a></p>'''+
        '<p>正文段落与保留的源位置。'+('Actual body text 中文。 '*600)+'</p></body></html>').encode()
    entries['OPS/note.xhtml']=b'<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><aside epub:type="footnote" id="n1"><p>Footnote payload.</p></aside></body></html>'
    book=out/'fixture.epub'
    with zipfile.ZipFile(book,'w',compression=zipfile.ZIP_DEFLATED) as z:
        for n,s in entries.items():z.writestr(n,s)
    results=[]
    for name,flags,spine,anchor,skip,note in [('first',163,4294967295,0,0,None),('body',35,1,0,0,None),
        ('restore',35,1,4096,0,None),('nav',65,1,0,0,None),('nav-resume',65,1,0,25,None),
        ('note',8,0,0,0,'OPS/note.xhtml#n1')]:
        command=[str(args.exe.resolve()),str(book),str(flags),str(spine),str(anchor),str(skip),str(out/(name+'.wire')),'0']
        if note:command.append(note)
        r=subprocess.run(command,capture_output=True,text=True,timeout=120)
        (out/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        print(name,r.stdout+r.stderr,flush=True);assert not r.returncode
        results.append(dict(case=name,passed=True))
    def capture(label, content, flags=163, expected=0):
        directory=out/label;directory.mkdir(exist_ok=True)
        path=directory/'fixture.epub'
        with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_DEFLATED) as z:
            for n,s in content.items():z.writestr(n,s)
        wire=directory/'first.wire'
        r=subprocess.run([str(args.exe.resolve()),str(path),str(flags),'4294967295','0','0',str(wire),str(expected)],
            capture_output=True,text=True,timeout=120)
        (directory/'first.log').write_text(r.stdout+r.stderr,encoding='utf-8')
        print(label,r.stdout+r.stderr,flush=True);assert not r.returncode
        results.append(dict(case=label,passed=True))
        return directory,list(wire_events(wire.read_bytes()))
    short=dict(entries)
    short['OPS/c1.xhtml']=b'<html xmlns="http://www.w3.org/1999/xhtml"><body><h1 id="title">Chapter 1</h1><p>A short real prose paragraph that reaches body EOF before the first page is full.</p></body></html>'
    directory,events=capture('short',short)
    for name,flags,anchor in [('body',35,0),('restore',35,0),('nav',65,0)]:
        r=subprocess.run([str(args.exe.resolve()),str(directory/'fixture.epub'),str(flags),'1',str(anchor),'0',
            str(directory/(name+'.wire')),'0'],capture_output=True,text=True,timeout=120)
        (directory/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');assert not r.returncode
    unmarked=dict(short)
    unmarked['OPS/toc.ncx']=b'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap/></ncx>'
    _,events=capture('unmarked',unmarked)
    assert [h[2] for h,r in events if h[0]==9 and h[4]]==[1]
    guide=dict(short)
    guide['OPS/book.opf']=guide['OPS/book.opf'].replace(b'</package>',b'<guide><reference type="text" href="c1.xhtml#body"/></guide></package>')
    guide['OPS/c1.xhtml']=b'<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Front material.</p><section id="body"><h1>Chapter 1</h1><p>Actual prose begins here.</p></section></body></html>'
    _,events=capture('guide-fragment',guide)
    target=next(i for i,(h,r) in enumerate(events) if h[0]==10 and h[5]&0x200)
    assert events[target][0][3]==len(b'Front material.')
    assert b'Front material.' not in b''.join(r[32:] for h,r in events[target:] if h[0]==1)
    landmarks=dict(guide)
    landmarks['OPS/book.opf']=short['OPS/book.opf'].replace(b'</manifest>',b'<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>')
    landmarks['OPS/nav.xhtml']=b'<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="landmarks"><ol><li><a epub:type="bodymatter" href="c1.xhtml#body">Start reading</a></li></ol></nav></body></html>'
    _,events=capture('landmark-fragment',landmarks)
    assert next(h[3] for h,r in events if h[0]==10 and h[5]&0x200)==len(b'Front material.')
    image_only=dict(short);image_only['OPS/c1.xhtml']=image_only['OPS/c0.xhtml']
    _,events=capture('image-only-entry',image_only)
    assert [h[2] for h,r in events if h[0]==9 and h[4]]==[2]
    directory=out/'image-restore';directory.mkdir(exist_ok=True)
    book=out/'image-only-entry/fixture.epub'
    for name,flags in [('first',35),('body',35),('restore',35),('nav',65)]:
        r=subprocess.run([str(args.exe.resolve()),str(book),str(flags),'1','0','0',str(directory/(name+'.wire')),'0'],
            capture_output=True,text=True,timeout=120)
        (directory/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');assert not r.returncode
    events=list(wire_events((directory/'body.wire').read_bytes()))
    assert any(h[0]==10 and h[5]&0x2000 for h,r in events) and not any(h[0]==1 for h,r in events)
    results.append(dict(case='image-body-fallback',passed=True))
    bounded=dict(unmarked)
    bounded['OPS/c0.xhtml']=b'<html><head><!--'+b' '*70000+b'--></head><body><p>Real prose after a long head.</p></body></html>'
    _,events=capture('bounded-probe',bounded)
    assert any(h[0]==1 and b'Real prose after a long head.' in r for h,r in events)
    comic=dict(image_only)
    for i in range(65):comic[f'OPS/c{i}.xhtml']=image_only['OPS/c0.xhtml']
    _,events=capture('image-book',comic)
    assert any(h[0]==6 for h,r in events) and any(h[0]==10 and h[5]&0x2000 for h,r in events)
    corrupt=dict(short);corrupt['OPS/c1.xhtml']=b'<html xmlns="http://www.w3.org/1999/xhtml"><body><broken></html>'
    directory,events=capture('recoverable-markup',corrupt)
    ordinary=directory/'ordinary.wire'
    r=subprocess.run([str(args.exe.resolve()),str(directory/'fixture.epub'),'35','1','0','0',str(ordinary),'0'],
        capture_output=True,text=True,timeout=120)
    (directory/'ordinary.log').write_text(r.stdout+r.stderr,encoding='utf-8');assert not r.returncode
    assert ordinary.read_bytes()==(directory/'first.wire').read_bytes()
    legacy=dict(short);legacy['OPS/c1.xhtml']=b'<html><body><p>Readable legacy HTML body without closing tags'
    _,events=capture('legacy-html-entry',legacy)
    assert any(h[0]==1 and b'Readable legacy HTML body' in r for h,r in events)
    (out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')


if __name__=='__main__':main()
