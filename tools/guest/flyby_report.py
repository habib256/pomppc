#!/usr/bin/env python3
"""Validate and summarize a completed utflyby job, using the same simulation frames."""
import argparse
import csv
import json
import math
from pathlib import Path
from frame_report import summarize


def check_capture(pixels, width, height):
    """Reject a capture that is not a rendered frame.

    'Not entirely black' accepted a single stray pixel, a flat desktop grey or
    a solid clear colour as proof that the game drew the scene (bug hunt T12).
    A real AS-Convoy frame has hundreds of distinct colours over most of the
    image; require a variety of colours AND that they cover the frame, so that
    a mostly-black capture with one bright corner still fails."""
    step = max(1, (width * height) // 20000)          # sample ~20k pixels
    sample = [pixels[i*3:i*3+3] for i in range(0, width * height, step)]
    colours = set(sample)
    lit = sum(1 for p in sample if p != b'\0\0\0')
    if len(colours) < 64 or lit < len(sample) // 2:
        raise ValueError('scanout capture does not look like a rendered frame: '
                         '%d distinct colours, %d/%d non-black samples'
                         % (len(colours), lit, len(sample)))
    return {'capture_colours': len(colours),
            'capture_lit_fraction': round(lit / len(sample), 4)}


def report(folder):
    folder = Path(folder)
    if not (folder / 'complete').is_file():
        raise ValueError('run incomplete')
    if (folder / 'exit-status.txt').read_text().strip() not in ('0', '143'):
        raise ValueError('game crashed during cleanup')
    manifest = dict(line.split('=', 1) for line in
                    (folder / 'manifest.txt').read_text().splitlines() if '=' in line)
    capture = (folder / 'frame-74.ppm').read_bytes().split(b'\n', 3)
    width, height = map(int, manifest['resolution'].split('x'))
    if (len(capture) != 4 or capture[0] != b'P6' or capture[2] != b'255' or
            capture[1] != ('%d %d' % (width, height)).encode() or
            len(capture[3]) != width*height*3):
        raise ValueError('missing, truncated or wrong-resolution scanout capture')
    capture_stats = check_capture(capture[3], width, height)
    first, last = int(manifest['first_frame']), int(manifest['last_frame'])
    if first < 2 or last <= first:
        raise ValueError('invalid frame window')
    expected = list(range(first, last + 1))
    with (folder / 'frames.csv').open(newline='') as stream:
        all_rows = list(csv.DictReader(stream))
    with (folder / 'clock.csv').open(newline='') as stream:
        all_clock = list(csv.DictReader(stream))
    # One render trace line per engine tick, over the WHOLE run and not just
    # the measured window: a driver that counts a frame twice (the Marble
    # Blast doubling) keeps the window consistent and still doubles the rate
    # (bug hunt T7).
    if len(all_rows) != len(all_clock):
        raise ValueError('render trace and engine clock disagree on frame count: '
                         '%d vs %d' % (len(all_rows), len(all_clock)))
    rows = [r for r in all_rows if first <= int(r['frame']) <= last]
    clock = [r for r in all_clock if first <= int(r['frame']) <= last]
    if [int(r['frame']) for r in rows] != expected:
        raise ValueError('missing, duplicate or unordered render frames')
    if [int(r['frame']) for r in clock] != expected:
        raise ValueError('missing, duplicate or unordered engine clock frames')
    ticks = [int(r['tick']) for r in clock]
    if ticks != list(range(ticks[0], ticks[0] + len(expected))):
        raise ValueError('measurement must contain exactly one simulation tick per swap')
    step = float(manifest['simulation_step_seconds'])
    if not 0 < step <= 1:
        raise ValueError('invalid simulation step')
    for r in clock:
        if (int(r['fixed']) != 1 or int(r['benchmark']) != 0 or
                not math.isfinite(float(r['step'])) or not math.isfinite(float(r['delta'])) or
                abs(float(r['step']) - step) > 1e-6 or
                abs(float(r['delta']) - step) > 1e-6):
            raise ValueError('engine did not maintain the requested fixed simulation step')
    if any(not math.isfinite(float(r['elapsed_ms'])) for r in rows):
        raise ValueError('non-finite frame timestamp')
    result = summarize(rows)
    result.update(scene=manifest['scene'], resolution=manifest['resolution'],
                  first_frame=first, last_frame=last,
                  simulation_step_seconds=step, simulated_seconds=(last-first)*step,
                  **capture_stats)
    for key in ('raw_vertices', 'raw_draws', 'fallbacks', 'readbacks'):
        delta = int(rows[-1][key]) - int(rows[0][key])
        if delta < 0:
            raise ValueError('render counters reset inside measurement window')
        result[key + '_per_frame'] = delta / (last-first)
    result['note'] = ('Same fixed simulation-frame window; application swap intervals, '
                      'not GPU completion. Verify camera captures and visual correctness '
                      'before treating results as equivalent rendering.')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    try:
        result = report(args.folder)
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
