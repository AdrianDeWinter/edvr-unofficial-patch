#!/usr/bin/env python3
"""Turn an eye dump (hotkey.dump_eyes: edvr_logs\\eyes\\eye_<stamp>_L.bmp) into
a PNG small enough to look at, optionally cropped.

    python tools/eye_bmp_to_png.py <eye.bmp> [out.png] [--scale 0.25]
                                   [--crop x y w h] [--dry-run]
    python tools/eye_bmp_to_png.py --self-test

Needs Pillow. The BMP is 24-bit bottom-up, some 75 MB at the DLAA size;
the PNG at a quarter scale is a few MB and shows what the player saw.
--dry-run reads the BMP, says what the PNG would be and where, and writes
nothing. --self-test converts a small BMP made in the temp folder: the
default name, the scale, the crop and the dry run.
"""
from __future__ import annotations

import argparse
import io
import os
import sys

import dump_io   # tools\dump_io.py: P and L0 may be NAME.bmp.z, an XOR delta against T and FinalCrisp_LOverview


def main(argv: list[str]) -> int:
    if '--self-test' in argv:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('bmp')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--scale', type=float, default=0.25)
    ap.add_argument('--crop', type=int, nargs=4, metavar=('X', 'Y', 'W', 'H'))
    ap.add_argument('--dry-run', action='store_true',
                    help='say what would be written and write nothing')
    a = ap.parse_args(argv)
    try:
        from PIL import Image
    except ImportError:
        print('Pillow is not installed: pip install pillow', file=sys.stderr)
        return 2
    Image.MAX_IMAGE_PIXELS = None
    if a.bmp.endswith(dump_io.Z_SUFFIX):
        a.bmp = a.bmp[:-len(dump_io.Z_SUFFIX)]   # named by its .z: the picture's name is the original's
    try:
        im = Image.open(io.BytesIO(dump_io.read_bytes(a.bmp)))   # a plain BMP, or a delta rebuilt against its base
    except (OSError, ValueError) as e:
        print('%s: %s' % (a.bmp, e), file=sys.stderr)
        return 1
    if a.crop:
        x, y, w, h = a.crop
        im = im.crop((x, y, x + w, y + h))
    if a.scale != 1.0:
        im = im.resize((max(1, int(im.width * a.scale)), max(1, int(im.height * a.scale))),
                       Image.Resampling.LANCZOS)
    out = a.out or os.path.splitext(a.bmp)[0] + ('_crop' if a.crop else '') + '.png'
    if not a.dry_run:
        im.save(out)
    print('%s -> %s (%dx%d)%s' % (a.bmp, out, im.width, im.height,
                                  ' (dry run: not written)' if a.dry_run else ''))
    return 0


def self_test() -> int:
    """A 64x32 BMP with a different colour in each corner, converted the ways
    the docs show; the PNG must be the size and the pixels the arguments say."""
    import contextlib
    import io
    import shutil
    import tempfile
    try:
        from PIL import Image
    except ImportError:
        print('eye_bmp_to_png: self-test needs Pillow: pip install pillow', file=sys.stderr)
        return 2

    failures = []

    def expect(name, condition, what=''):
        if not condition:
            failures.append('%s: %s' % (name, what))

    def run(*argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = main(list(argv))
        return code, out.getvalue()

    base = tempfile.mkdtemp(prefix='edvr-eye-bmp-')
    try:
        bmp = os.path.join(base, 'eye_120000_L.bmp')
        src = Image.new('RGB', (64, 32), (10, 20, 30))
        src.putpixel((0, 0), (255, 0, 0))
        src.putpixel((63, 0), (0, 255, 0))
        src.putpixel((0, 31), (0, 0, 255))
        src.putpixel((63, 31), (255, 255, 0))
        src.save(bmp)

        # Default: a quarter scale, named beside the BMP.
        png = os.path.join(base, 'eye_120000_L.png')
        code, said = run(bmp)
        expect('default', code == 0 and said == '%s -> %s (16x8)\n' % (bmp, png), '%r %r' % (code, said))
        expect('default', os.path.isfile(png) and Image.open(png).size == (16, 8), 'the PNG')

        # Full scale keeps the pixels; an explicit name is used as given.
        full = os.path.join(base, 'full.png')
        code, said = run(bmp, full, '--scale', '1')
        got = Image.open(full).convert('RGB')
        expect('scale-1', code == 0 and got.size == (64, 32) and got.getpixel((0, 0)) == (255, 0, 0)
               and got.getpixel((63, 31)) == (255, 255, 0) and got.getpixel((5, 5)) == (10, 20, 30),
               '%r %r %r' % (code, said, got.size))

        # A crop: the region asked for, named with _crop, and at scale 1 pixel for pixel.
        code, said = run(bmp, '--scale', '1', '--crop', '48', '0', '16', '8')
        crop = os.path.join(base, 'eye_120000_L_crop.png')
        expect('crop', code == 0 and '(16x8)' in said and os.path.isfile(crop), '%r %r' % (code, said))
        cropped = Image.open(crop).convert('RGB')
        expect('crop', cropped.size == (16, 8) and cropped.getpixel((15, 0)) == (0, 255, 0)
               and cropped.getpixel((0, 0)) == (10, 20, 30), 'the cropped pixels')

        # A P picture stored as an XOR delta against its T twin (P##.bmp.z), as the eye dump writes it: the PNG is P's own pixels,
        # named by either spelling; with the base gone it says so and exits 1.
        twin = os.path.join(base, 'eye_120000_T00.bmp')
        src.save(twin)
        pre = src.copy()
        pre.putpixel((10, 10), (200, 100, 50))
        pbmp = os.path.join(base, 'eye_120000_P00.bmp')
        pre.save(pbmp)
        with open(pbmp, 'rb') as fh:
            pbytes = fh.read()
        dump_io.write_z(pbmp, pbytes, base=twin)
        os.remove(pbmp)
        for spelling in (pbmp, pbmp + '.z'):
            code, said = run(spelling, os.path.join(base, 'p.png'), '--scale', '1')
            pgot = Image.open(os.path.join(base, 'p.png')).convert('RGB')
            expect('delta', code == 0 and pgot.getpixel((10, 10)) == (200, 100, 50) and pgot.getpixel((0, 0)) == (255, 0, 0), '%r %r' % (code, said))
        os.remove(twin)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            code, said = run(pbmp, os.path.join(base, 'p2.png'))
        expect('delta', code == 1 and not os.path.exists(os.path.join(base, 'p2.png')) and 'base' in err.getvalue(), 'a delta without its base: %r %r' % (code, err.getvalue()))

        # A dry run reads and reports and writes nothing: no PNG, no directory.
        before = sorted(os.listdir(base))
        code, said = run(bmp, '--scale', '0.5', '--dry-run')
        expect('dry-run', code == 0 and '(32x16) (dry run: not written)' in said, '%r %r' % (code, said))
        expect('dry-run', sorted(os.listdir(base)) == before, '--dry-run wrote %r'
               % sorted(set(os.listdir(base)) - set(before)))
        target = os.path.join(base, 'not-made', 'out.png')
        code, said = run(bmp, target, '--dry-run')
        expect('dry-run', code == 0 and not os.path.exists(os.path.dirname(target)), '--dry-run made a directory')
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print('eye_bmp_to_png: self-test FAILED')
        for f in failures:
            print('  ' + f.replace('\n', '\n    '))
        return 1
    print('eye_bmp_to_png: self-test OK')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
