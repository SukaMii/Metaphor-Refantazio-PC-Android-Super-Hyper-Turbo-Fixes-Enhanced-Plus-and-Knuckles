"""Builds the release zip from build\\ after build.ps1: the mod, its INI, the same INI with the Steam Deck profile set, and the
licences. Writes dist\\MetaphorCPU-<version>.zip and prints its SHA-256.

Usage: python tools\\package_release.py
"""
import hashlib
import os
import re
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = re.search(r'#define MCPU_VERSION "([\d.]+)"', open(os.path.join(ROOT, 'src', 'mcpu.h'), encoding='utf-8').read()).group(1)
ITEMS = [('build/MetaphorCPU.asi', 'MetaphorCPU.asi'),
         ('build/MetaphorCPU.ini', 'MetaphorCPU.ini'),
         ('build/Steam Deck/MetaphorCPU.ini', 'Steam Deck/MetaphorCPU.ini'),
         ('LICENSE', 'licences/LICENSE-MetaphorCPU.txt'),
         ('third_party/meshoptimizer/LICENSE.md', 'licences/LICENSE-meshoptimizer.md')]


def main():
    asi = open(os.path.join(ROOT, 'build', 'MetaphorCPU.asi'), 'rb').read()
    if ('MetaphorCPU ' + VERSION).encode() not in asi and VERSION.encode() not in asi:
        raise SystemExit('build\\MetaphorCPU.asi is not version %s: run build.ps1 first' % VERSION)
    deck = open(os.path.join(ROOT, 'build', 'Steam Deck', 'MetaphorCPU.ini'), encoding='utf-8').read()
    if not re.search(r'(?m)^Profile=steamdeck\r?$', deck):
        raise SystemExit('build\\Steam Deck\\MetaphorCPU.ini does not set Profile=steamdeck')
    os.makedirs(os.path.join(ROOT, 'dist'), exist_ok=True)
    out = os.path.join(ROOT, 'dist', 'MetaphorCPU-%s.zip' % VERSION)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for src, name in ITEMS:
            z.write(os.path.join(ROOT, src), name)
    with zipfile.ZipFile(out) as z:
        if z.testzip() is not None:
            raise SystemExit('the zip did not read back')
        print('\n'.join('  %-36s %8d bytes' % (i.filename, i.file_size) for i in z.infolist()))
    print('%s\nSHA-256 %s' % (out, hashlib.sha256(open(out, 'rb').read()).hexdigest()))


if __name__ == '__main__':
    main()
