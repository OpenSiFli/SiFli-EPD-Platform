"""Summarize complete worker-published seven-page records from a saved serial log."""
from pathlib import Path
import argparse,json,re

def fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)',line))

def result(item):
    h=item['header'];rows=item['rows'];reasons=[]
    check=lambda flag,reason: reasons.append(reason) if not flag else None
    check(h.get('current')=='1','snapshot is stale or unavailable')
    check(h.get('ready')=='1','worker target is not ready')
    check(h.get('published')=='1' and h.get('published_intent')==h.get('intent'),'target is not published for this intent')
    check(h.get('hot')=='7','worker counted fewer than seven distinct hot pages')
    check(len(rows)==7 and {r.get('rel') for r in rows}=={str(i) for i in range(-3,4)},'incomplete or duplicated relative-page records')
    check(all(r.get('serial')==h.get('serial') for r in rows),'mixed publication serials')
    check(all(all(r.get(k)=='1' for k in ('mapped','known','present','complete','pixelsready','hot'))
              and r.get('cold')=='0' and r.get('cancelled')=='0' for r in rows),'one or more pages are not hot')
    check(len({(r.get('context'),r.get('layout'),r.get('cursor')) for r in rows})==7,'fewer than seven distinct page identities')
    center=next((r for r in rows if r.get('rel')=='0'),{})
    check(center.get('generation')==h.get('published_generation'),'current frame differs from published generation')
    return {**item,'seven_hot_at_sample':not reasons,'reasons':reasons}

def parse(text):
    items=[];current=None
    for number,line in enumerate(text.splitlines(),1):
        if 'reader window_snapshot ' in line:
            if current:items.append(result(current))
            current={'line':number,'header':fields(line),'rows':[]}
        elif 'reader window_page ' in line and current:
            current['rows'].append(fields(line))
    if current:items.append(result(current))
    return items

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args();items=parse(a.log.read_text(encoding='utf-8',errors='replace'))
    out={'source':str(a.log.resolve()),'snapshots':items,'seven_hot_samples':sum(i['seven_hot_at_sample'] for i in items)}
    text=json.dumps(out,ensure_ascii=False,indent=2)+'\n'
    if a.output:a.output.write_text(text,encoding='utf-8')
    print(text)
