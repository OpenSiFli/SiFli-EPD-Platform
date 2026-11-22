"""Check snapshot log acceptance and rejection without a device connection."""
import argparse
import json
from pathlib import Path
from parse_snapshots import parse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    header = ('reader window_snapshot session=3 intent=4 serial=1 tick=100 age_ms=1 current=1 '
              'ready=1 hot=7 published=1 published_intent=4 published_generation=2 paused=0 suspended=0\n')
    rows = ''.join(f'reader window_page serial=1 rel={i} context=0x1234 chapter=8 page={10+i} '
                   f'layout=1 generation=2 cursor={10+i}:0 end={11+i}:0 mapped=1 known=1 '
                   'present=1 complete=1 pixelsready=1 cold=0 cancelled=0 hot=1\n' for i in range(-3, 4))
    positive = header + rows
    assert parse(positive)[0]['seven_hot_at_sample']
    mutations = {
        'stale': positive.replace('current=1', 'current=0', 1),
        'duplicate': positive.replace('rel=3 ', 'rel=2 ', 1),
        'cold': positive.replace('cold=0', 'cold=1', 1),
        'generation': positive.replace('published_generation=2', 'published_generation=3', 1),
        'serial': positive.replace('window_page serial=1', 'window_page serial=2', 1),
        'incomplete': '\n'.join(line for line in positive.splitlines() if 'rel=3 ' not in line),
        'unpublished': positive.replace('published=1', 'published=0', 1),
        'six_hot': positive.replace('hot=7', 'hot=6', 1),
        'cancelled': positive.replace('cancelled=0', 'cancelled=1', 1),
        'same_page': positive.replace('cursor=13:0', 'cursor=12:0', 1),
    }
    for name, text in mutations.items():
        assert not parse(text)[0]['seven_hot_at_sample'], name
    assert parse('no snapshot here') == []
    report = {'positive': True, 'negative_controls': list(mutations), 'no_snapshot': True}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS seven-hot parser positive, ten negative controls and absent snapshot')


if __name__ == '__main__':
    main()
