#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The game's Options menu "Graphics" tab: relabelled menu text in an extra archive, update3.pak.

The Options menu (ui/Octane_Options_Menu.igz) has a fifth tab, Language Select, that the game
hides in this region. The port shows it again and opens its own settings menu when it is chosen
(gpu/shim/bbport_graphics_tab.cpp); this script renames it "Graphics" in the menu's four text
tables (ui/Octane_Options_Menu_{en,fr,mx,pt}.lng).

The game loads archives/update.pak, update2.pak, ... until one is missing, each overriding the
files of the ones before; the 1.21 update's own menu files are in update.pak/update2.pak. The
newest copy of each text table is read from the player's own archives, relabelled, and written
to a small update3.pak (uncompressed, the archives' own format) that the game loads last.

Diagnostics: the Options menu files of every archive searched are copied to
OUT/graphics-tab/dump/<archive>/ (a few hundred kilobytes).

Prints "archives/update3.pak=<file>" for scripts/mods.py --replace, or nothing when the
graphics_tab setting is off or the files are not found.

usage: graphics_tab.py GAME --out OUT [--settings bbport.ini]
"""
import argparse
import lzma
import struct
import sys
import zlib
from pathlib import Path

# Searched newest first: the first archive that has a file wins.
ARCHIVES = ('update2.pak', 'update.pak', 'juicedomain_permanent.pak')
OUTPUT = 'archives/update3.pak'
# The tab's label is the string before this placeable name in each text table.
ANCHOR = b'Comp__Menu_ButtonWithIcon005_behaviors_0'
LABELS = {'en': 'Graphics', 'fr': 'Graphismes', 'mx': 'Gráficos', 'pt': 'Gráficos'}
DUMP = ('octane_options_menu', 'octane_options_languageselect')
SECTOR = 0x800
VERSION = 3  # of this script's output (the cached file is rebuilt when it changes)


def fnv1a(text):
    h = 0x811C9DC5
    for c in text.lower().encode('utf-8'):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


class Archive:
    """An igArchive (IGA version 13) read in place: the header and tables, then file data."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(0x38)
        (magic, version, _toc, count, sector, self.divider, self.slop, n_large, n_medium, n_small,
         name_off, name_size, self.flags) = struct.unpack('<IIIIIIIIIIQII', head)
        if magic != 0x1A414749 or version != 13 or sector != SECTOR:
            raise ValueError(f'{path.name}: unexpected archive header')
        tables = self.f.read(4 * count + 16 * count + 4 * n_large + 2 * n_medium + n_small)
        pos = 4 * count
        self.infos = [struct.unpack_from('<IiIi', tables, pos + 16 * i) for i in range(count)]
        pos += 16 * count
        self.large = struct.unpack_from(f'<{n_large}I', tables, pos); pos += 4 * n_large
        self.medium = struct.unpack_from(f'<{n_medium}H', tables, pos); pos += 2 * n_medium
        self.small = tables[pos:pos + n_small]
        self.f.seek(name_off)
        names = self.f.read(name_size)
        self.names = []
        for i in range(count):
            o = struct.unpack_from('<i', names, 4 * i)[0]
            end = names.index(b'\0', o)
            end2 = names.index(b'\0', end + 1)
            self.names.append((names[o:end].decode('utf-8', 'replace'),
                               names[end + 1:end2].decode('utf-8', 'replace')))

    def logical(self, i):
        full, logical = self.names[i]
        return (logical or full).replace('\\', '/')

    def read(self, i):
        offset, ordinal, size, block_index = self.infos[i]
        offset |= (ordinal & 0xFF) << 32  # archives past 4 GiB: the offset's high byte
        if block_index == -1:
            self.f.seek(offset)
            return self.f.read(size)
        comp = (block_index & 0xFFFFFFFF) >> 28
        first = block_index & 0x0FFFFFFF
        out = bytearray(size)
        for b in range((size + 0x7FFF) >> 15):
            bi = first + b
            if 0x7F * SECTOR < size:
                if 0x7FFF * SECTOR < size:
                    e, n, mask, bit = self.large[bi], self.large[bi + 1], 0x7FFFFFFF, 31
                else:
                    e, n, mask, bit = self.medium[bi], self.medium[bi + 1], 0x7FFF, 15
            else:
                e, n, mask, bit = self.small[bi], self.small[bi + 1], 0x7F, 7
            boff = (e & mask) * SECTOR
            bsize = (n & mask) * SECTOR - boff
            dsize = min(0x8000, size - b * 0x8000)
            self.f.seek(offset + boff)
            src = self.f.read(bsize)
            if not (e >> bit) & 1:
                chunk = src[:dsize]
            elif comp == 1:
                csize = struct.unpack_from('<H', src, 0)[0]
                chunk = zlib.decompress(src[2:2 + csize], -15)
            elif comp == 2:
                csize = struct.unpack_from('<I', src, 0)[0]
                alone = src[4:9] + struct.pack('<Q', dsize) + src[9:9 + csize]
                chunk = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(alone, dsize)
            else:
                raise ValueError(f'compression {comp}')
            out[b * 0x8000:b * 0x8000 + dsize] = chunk[:dsize]
        return bytes(out)


def relabel(text, label):
    k = text.find(ANCHOR)
    if k < 0 or text.count(ANCHOR) != 1:
        raise ValueError('tab label not found')
    j = k - 1
    while text[j] == 0:
        j -= 1
    start = text.rfind(b'\0', 0, j) + 1
    room = k - start  # the old label and its padding
    new = label.encode('utf-8') + b'\0'
    if len(new) > room:
        raise ValueError('label too long')
    return text[:start] + new.ljust(room, b'\0') + text[k:]


def write_archive(dest, files, template):
    """files: [(full name, logical name, ordinal, data)], uncompressed (block index -1)."""
    files = sorted(files, key=lambda f: fnv1a(f[1]))
    count = len(files)
    names = bytearray(4 * count)
    for i, (full, logical, _ordinal, _data) in enumerate(files):
        struct.pack_into('<i', names, 4 * i, len(names))
        names += full.encode('utf-8') + b'\0' + logical.encode('utf-8') + b'\0' + b'\0' * 4
    toc_size = 4 * count + 16 * count
    data_start = (0x38 + toc_size + SECTOR - 1) // SECTOR * SECTOR
    infos = bytearray()
    body = bytearray()
    for full, logical, ordinal, data in files:
        infos += struct.pack('<IiIi', data_start + len(body), ordinal, len(data), -1)
        body += data + b'\0' * (-len(data) % SECTOR)
    name_off = data_start + len(body)
    # The lookup: an id's expected position is id // divider, searched within +-slop
    # (essentialui.pak, 4 files: divider 0x3fffffff).
    divider = (1 << 32) // count - 1
    slop = max(abs(i - fnv1a(f[1]) // divider) for i, f in enumerate(files)) + 1
    head = struct.pack('<IIIIIIIIIIQII', 0x1A414749, 13, toc_size, count, SECTOR, divider,
                       slop, 0, 0, 0, name_off, len(names), template.flags)
    toc = b''.join(struct.pack('<I', fnv1a(f[1])) for f in files) + infos
    out = bytearray(head + toc)
    out += b'\0' * (data_start - len(out))
    out += body + names
    tmp = dest.with_suffix('.tmp')
    tmp.write_bytes(out)
    tmp.replace(dest)


def build(game, dest, dump):
    found = {}  # language -> (archive, index)
    archives = []
    for name in ARCHIVES:
        path = game / 'archives' / name
        if not path.is_file():
            continue
        archive = Archive(path)
        archives.append(archive)
        for i in range(len(archive.names)):
            logical = archive.logical(i)
            low = logical.lower()
            if any(d in low for d in DUMP):
                target = dump / name / logical
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(i))
            for lang in LABELS:
                if low == f'ui/octane_options_menu_{lang}.lng' and lang not in found:
                    found[lang] = (archive, i, name)
    if len(found) != len(LABELS):
        raise ValueError(f'{len(found)} of {len(LABELS)} menu text tables found')
    files = []
    for lang, (archive, i, name) in found.items():
        full, logical = archive.names[i]
        files.append((full, logical, archive.infos[i][1] & ~0xFF, relabel(archive.read(i), LABELS[lang])))
        print(f'Graphics tab: {logical} from {name}')
    dest.parent.mkdir(parents=True, exist_ok=True)
    write_archive(dest, files, archives[0])


def enabled(settings):
    if not settings or not Path(settings).is_file():
        return True
    for line in Path(settings).read_text(encoding='utf-8', errors='replace').splitlines():
        key, _, value = line.partition('=')
        if key.strip() == 'graphics_tab':
            return value.strip() != '0'
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--settings', type=Path)
    args = parser.parse_args()
    if not enabled(args.settings):
        return
    if (args.game / OUTPUT).exists():
        print(f'Graphics tab: the game already has {OUTPUT}, off')
        return
    stamp = '-'.join(f'{(args.game / "archives" / a).stat().st_size}'
                     for a in ARCHIVES if (args.game / 'archives' / a).is_file())
    folder = args.out / 'graphics-tab'
    dest = folder / f'update3-v{VERSION}-{stamp}.pak'
    try:
        if not dest.is_file():
            for old in folder.glob('*.pak'):
                old.unlink()
            build(args.game, dest, folder / 'dump')
    except (OSError, ValueError, lzma.LZMAError, zlib.error) as error:
        print(f'Graphics tab: {error}, off')
        return
    print(f'Graphics tab: Options menu tab relabelled ({dest.name})')
    print(f'{OUTPUT}={dest}')


if __name__ == '__main__':
    main()
