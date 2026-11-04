"""Check small visible effects that a whole-frame average can conceal.

Regions belong to one exact CPU capture and one reference RGB image. Unknown
captures have no registered regions; they are not claimed as effect coverage.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image


def check_regions(rendered, reference, case):
    if rendered.shape != reference.shape:
        raise ValueError('Effect images have different dimensions')
    if hashlib.sha256(reference.tobytes()).hexdigest() != case['reference_rgb_sha256']:
        raise ValueError('Effect reference belongs to a different frame')
    rows = []
    for region in case['regions']:
        left, top, right, bottom = region['box']
        if not (0 <= left < right <= rendered.shape[1] and 0 <= top < bottom <= rendered.shape[0]):
            raise ValueError('Effect region is outside the image')
        error = np.abs(rendered[top:bottom, left:right].astype(float) - reference[top:bottom, left:right])
        maximum, mean = float(error.max()), float(error.mean())
        rows.append(dict(name=region['name'], box=region['box'], maximum=maximum, mean=mean,
                         passed=maximum <= region['maximum_error'] and mean <= region['mean_error']))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rendered', type=Path)
    parser.add_argument('reference', type=Path)
    parser.add_argument('replay_index', type=Path)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    fixtures = json.loads((Path(__file__).resolve().parents[1] / 'tests/native_effect_regions.json').read_text())
    identity = json.loads(args.replay_index.read_text())['capture_sha256']
    case = fixtures.get(identity)
    rows = check_regions(np.array(Image.open(args.rendered).convert('RGB')),
                         np.array(Image.open(args.reference).convert('RGB')), case) if case else []
    report = dict(capture_sha256=identity, regions=rows, checked=len(rows), passed=all(r['passed'] for r in rows))
    if args.json:
        args.json.write_text(json.dumps(report, indent=2))
    for row in rows:
        print('%s: max %.3f, mean %.3f (%s)' % (row['name'], row['maximum'], row['mean'], 'pass' if row['passed'] else 'FAIL'))
    if not rows:
        print('No registered effect regions for this capture')
    if not report['passed']:
        raise SystemExit('Native effect region differs from the synchronized reference')


if __name__ == '__main__':
    main()
