#!/usr/bin/env python3
"""Correlate the recorded game mix with the moments an operator marked.

The capture tool (capture.ps1) writes mix.wav -- what the port sent to the audio
device -- and markers.json -- when a person pressed a marker key while riding.
This reads both and answers the only question that matters before building
anything: does the music change when this happens, and how long after?

    python analyse.py <capture dir>            # mix.wav + markers.json
    python analyse.py mix.wav --markers m.json
    python analyse.py --selftest

No scipy: features come from cumulative sums of the signal and its first
difference, which is enough to find arrangement changes and bar boundaries.
"""
import argparse
import json
import os
import sys
import wave

import numpy as np

MARKER_LABELS = {
    'jump': 'the rider leaves the ground',
    'land': 'the rider lands',
    'bail': 'the rider crashes',
    'trick': 'a trick lands',
    'finish': 'the run ends',
    'zone': 'the rider enters a marked part of the course',
}


def read_wav(path):
    with wave.open(path, 'rb') as w:
        frames, ch, rate, width = w.getnframes(), w.getnchannels(), w.getframerate(), w.getsampwidth()
        data = w.readframes(frames)
    if width != 2:
        raise SystemExit('%s: expected 16-bit PCM, got %d bytes per sample' % (path, width))
    x = np.frombuffer(data, dtype='<i2').astype(np.float32) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, rate


def features(x, rate, hop_s=0.02, win_s=0.05):
    hop, win = int(rate * hop_s), int(rate * win_s)
    hop = max(1, win // 3) if hop >= win else hop
    n = 1 + max(0, (len(x) - win) // hop)
    idx = np.arange(n) * hop

    energy = np.empty(n, dtype=np.float32)
    hfc = np.empty(n, dtype=np.float32)
    zcr = np.empty(n, dtype=np.float32)
    d = np.abs(np.diff(x, prepend=x[:1]))

    ce = np.concatenate(([0.0], np.cumsum(x * x, dtype=np.float64)))
    cd = np.concatenate(([0.0], np.cumsum(d, dtype=np.float64)))
    sign = np.signbit(x).astype(np.int8)
    cz = np.concatenate(([0.0], np.cumsum(np.abs(np.diff(sign, prepend=sign[:1])) > 0)))

    energy[:] = (ce[idx + win] - ce[idx]) / win
    hfc[:] = (cd[idx + win] - cd[idx]) / win
    zcr[:] = (cz[idx + win] - cz[idx]) / win
    t = (idx + win / 2.0) / rate
    return t, energy, hfc, zcr


def onset_curve(energy, hfc):
    """Positive change of level plus high-frequency content, normalised."""
    def flux(v):
        d = np.diff(v, prepend=v[:1])
        d[d < 0] = 0
        s = d.std() or 1.0
        return d / s
    return flux(np.log1p(energy * 1000.0)) + flux(hfc)


def bar_period(onset, hop_s):
    """The strongest periodic spacing of onsets, 0.4-2.5 s."""
    a = onset - onset.mean()
    lo, hi = max(2, int(0.4 / hop_s)), min(len(a) // 3, int(2.5 / hop_s))
    if hi <= lo:
        return None
    ac = np.array([np.dot(a[:-k], a[k:]) for k in range(lo, hi)], dtype=np.float64)
    if not len(ac) or ac.max() <= 0:
        return None
    k = int(np.argmax(ac)) + lo
    return k * hop_s


def novelty(t, energy, hfc, zcr, hop_s, span_s=1.2):
    """How much the sound at t differs from the sound `span` earlier."""
    span = max(1, int(span_s / hop_s))
    feat = np.stack([np.log1p(energy * 1000.0), np.log1p(hfc * 100.0), zcr * 100.0], axis=1)
    mu = feat.mean(axis=0) + 1e-6
    sd = feat.std(axis=0) + 1e-6
    z = (feat - mu) / sd
    d = np.full(len(z), 0.0)
    d[span:] = np.sqrt(((z[span:] - z[:-span]) ** 2).mean(axis=1))
    return d


def correlate(t, nov, markers, hop_s, window_s=2.0):
    rows = []
    for m in markers:
        t0 = m['t']
        sel = (t >= t0 - window_s) & (t <= t0 + window_s)
        if not sel.any():
            continue
        idx = np.flatnonzero(sel)
        best = idx[int(np.argmax(nov[sel]))]
        # a change is a plateau: report where it starts, not where it peaks
        peak = nov[best]
        while best > 0 and nov[best - 1] >= 0.6 * peak:
            best -= 1
        rows.append({
            'label': m.get('label', '?'),
            'at': round(t0, 3),
            'peak': round(float(t[best]), 3),
            'latency': round(float(t[best] - t0), 3),
            'strength': round(float(nov[best]), 3),
            'floor': round(float(nov[sel].mean()), 3),
        })
    return rows


def summarise(rows):
    out = {}
    for r in rows:
        out.setdefault(r['label'], []).append(r)
    table = []
    for label, rs in sorted(out.items()):
        lats = sorted(abs(x['latency']) for x in rs)
        med = lats[len(lats) // 2]
        strong = [x for x in rs if x['strength'] > 2.0 * (x['floor'] + 1e-6)]
        table.append({
            'label': label,
            'what': MARKER_LABELS.get(label, ''),
            'marks': len(rs),
            'median_latency': round(med, 3),
            'reacts': len(strong),
            'median_strength': round(sorted(x['strength'] for x in rs)[len(rs) // 2], 3),
            'floor': round(float(np.median([x['floor'] for x in rs])), 3),
        })
    return table


def report(path_dir, wav=None, markers=None, out_json=None):
    wav = wav or os.path.join(path_dir, 'mix.wav')
    markers = markers or os.path.join(path_dir, 'markers.json')
    x, rate = read_wav(wav)
    hop_s = 0.02
    t, energy, hfc, zcr = features(x, rate, hop_s=hop_s)
    onset = onset_curve(energy, hfc)
    nov = novelty(t, energy, hfc, zcr, hop_s)
    period = bar_period(onset, hop_s)

    marks = []
    if os.path.exists(markers):
        with open(markers) as f:
            marks = json.load(f)
    rows = correlate(t, nov, marks, hop_s)
    table = summarise(rows)

    print('mix        %s  %.1f s, %d Hz' % (os.path.basename(wav), len(x) / rate, rate))
    print('bar period %s' % ('%.3f s' % period if period else 'not found'))
    print('markers    %d in %d labels' % (len(marks), len(table)))
    if table:
        print()
        print('%-8s %-40s %5s %9s %7s %9s' % ('moment', 'what', 'marks', 'latency', 'reacts', 'strength'))
        for r in table:
            print('%-8s %-40s %5d %8.3fs %4d/%-2d %9.2f (floor %.2f)' % (
                r['label'], r['what'][:40], r['marks'], r['median_latency'],
                r['reacts'], r['marks'], r['median_strength'], r['floor']))
    result = {'wav': wav, 'rate': rate, 'seconds': len(x) / rate,
              'bar_period': period, 'table': table, 'marks': rows}
    if out_json:
        with open(out_json, 'w') as f:
            json.dump(result, f, indent=2)
        print('\nwrote %s' % out_json)
    return result


def selftest():
    """A synthetic mix: texture A, a 1.5 s gap, texture B. A change at 6.0 s and a
    marker at 5.9 s must be paired with a latency near 0.1 s."""
    rate = 48000
    dur = 12.0
    n = int(rate * dur)
    t = np.arange(n) / rate
    rng = np.random.default_rng(7)
    a = 0.2 * np.sin(2 * np.pi * 110 * t) + 0.05 * rng.standard_normal(n)
    b = 0.25 * np.sin(2 * np.pi * 330 * t) + 0.25 * rng.standard_normal(n)
    x = np.where(t < 6.0, a, b).astype(np.float32)
    hop_s = 0.02
    tt, energy, hfc, zcr = features(x, rate, hop_s=hop_s)
    nov = novelty(tt, energy, hfc, zcr, hop_s)
    rows = correlate(tt, nov, [{'t': 5.9, 'label': 'jump'}], hop_s)
    assert rows, 'no correlation row'
    r = rows[0]
    ok = 0.0 <= r['latency'] <= 0.4 and r['strength'] > 1.0
    print('selftest: change at 6.0 s, marker at 5.9 s -> peak %.2f s, latency %+.2f s, strength %.2f'
          % (r['peak'], r['latency'], r['strength']))
    print('selftest', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture', nargs='?', help='directory with mix.wav and markers.json')
    ap.add_argument('--wav')
    ap.add_argument('--markers')
    ap.add_argument('--json', dest='out_json')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.capture and not a.wav:
        ap.error('give a capture directory or --wav')
    d = a.capture or '.'
    out = a.out_json or os.path.join(d, 'analysis.json')
    report(d, a.wav, a.markers, out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
