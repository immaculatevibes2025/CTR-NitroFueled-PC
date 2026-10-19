#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The game's Options menu "Graphics" tab: a relabelled copy of archives/juicedomain_permanent.pak.

The Options menu (ui/Octane_Options_Menu.igz) has a fifth tab, Language Select, that the game
hides in this region. The port shows it again and opens its own settings menu when it is chosen
(gpu/shim/bbport_graphics_tab.cpp); this script renames it "Graphics" in the menu's four text
tables (ui/Octane_Options_Menu_{en,fr,mx,pt}.lng).

The copy is made from the player's own game files: the changed text tables are appended to the
archive uncompressed and their table entries point there; nothing else in the archive changes.
Prints "archives/juicedomain_permanent.pak=<copy>" for scripts/mods.py --replace, or nothing when
the graphics_tab setting is off or the archive is not the expected one.

usage: graphics_tab.py GAME --out OUT [--settings bbport.ini]
"""
import argparse
import lzma
import struct
import sys
import zlib
from pathlib import Path

ARCHIVE = 'archives/juicedomain_permanent.pak'
# The tab's label is the string before this placeable name in each text table.
ANCHOR = b'Comp__Menu_ButtonWithIcon005_behaviors_0'
LABELS = {'en': 'Graphics', 'fr': 'Graphismes', 'mx': 'Gráficos', 'pt': 'Gráficos'}
SECTOR = 0x800


def parse(data):
    (magic, version, _toc, count, sector, _div, _slop, n_large, n_medium, n_small,
     name_off, _name_size, _flags) = struct.unpack_from('<IIIIIIIIIIQII', data, 0)
    if magic != 0x1A414749 or version != 13 or sector != SECTOR:
        raise ValueError('unexpected archive header')
    pos = 0x38 + 4 * count
    info_at = pos
    infos = [struct.unpack_from('<IiIi', data, pos + 16 * i) for i in range(count)]
    pos += 16 * count
    large = struct.unpack_from(f'<{n_large}I', data, pos); pos += 4 * n_large
    medium = struct.unpack_from(f'<{n_medium}H', data, pos); pos += 2 * n_medium
    small = data[pos:pos + n_small]
    names = []
    for i in range(count):
        o = name_off + struct.unpack_from('<i', data, name_off + 4 * i)[0]
        end = data.index(b'\0', o)
        end2 = data.index(b'\0', end + 1)
        logical = data[end + 1:end2] or data[o:end]
        names.append(logical.decode('utf-8', 'replace').replace('\\', '/'))
    return info_at, infos, names, (large, medium, small)


def read_file(data, info, tables):
    offset, _ordinal, size, block_index = info
    if block_index == -1:
        return bytes(data[offset:offset + size])
    large, medium, small = tables
    comp = (block_index & 0xFFFFFFFF) >> 28
    first = block_index & 0x0FFFFFFF
    out = bytearray(size)
    for b in range((size + 0x7FFF) >> 15):
        bi = first + b
        if 0x7F * SECTOR < size:
            if 0x7FFF * SECTOR < size:
                e, n, mask, bit = large[bi], large[bi + 1], 0x7FFFFFFF, 31
            else:
                e, n, mask, bit = medium[bi], medium[bi + 1], 0x7FFF, 15
        else:
            e, n, mask, bit = small[bi], small[bi + 1], 0x7F, 7
        boff = (e & mask) * SECTOR
        bsize = (n & mask) * SECTOR - boff
        dsize = min(0x8000, size - b * 0x8000)
        src = data[offset + boff:offset + boff + bsize]
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


def build(source, dest):
    data = bytearray(source.read_bytes())
    info_at, infos, names, tables = parse(data)
    appended = bytearray()
    base = (len(data) + SECTOR - 1) // SECTOR * SECTOR
    changed = 0
    for i, name in enumerate(names):
        lang = next((l for l in LABELS if name.lower().endswith(f'ui/octane_options_menu_{l}.lng')), None)
        if not lang:
            continue
        text = relabel(read_file(data, infos[i], tables), LABELS[lang])
        offset = base + len(appended)
        appended += text
        appended += b'\0' * (-len(appended) % SECTOR)
        _off, ordinal, size, _block = infos[i]
        struct.pack_into('<IiIi', data, info_at + 16 * i, offset, ordinal, size, -1)
        changed += 1
    if changed != len(LABELS):
        raise ValueError(f'{changed} of {len(LABELS)} text tables found')
    data += b'\0' * (base - len(data))
    data += appended
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix('.tmp')
    tmp.write_bytes(data)
    tmp.replace(dest)


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
    source = args.game / ARCHIVE
    if not source.is_file():
        print(f'Graphics tab: no {ARCHIVE}, off', file=sys.stderr)
        return
    st = source.stat()
    dest = args.out / 'graphics-tab' / f'juicedomain_permanent-{st.st_size}-{int(st.st_mtime)}.pak'
    try:
        if not dest.is_file():
            for old in dest.parent.glob('*.pak'):
                old.unlink()
            build(source, dest)
    except (OSError, ValueError, lzma.LZMAError, zlib.error) as error:
        print(f'Graphics tab: {error}, off', file=sys.stderr)
        return
    print(f'Graphics tab: Options menu tab relabelled ({dest.name})', file=sys.stderr)
    print(f'{ARCHIVE}={dest}')


if __name__ == '__main__':
    main()
