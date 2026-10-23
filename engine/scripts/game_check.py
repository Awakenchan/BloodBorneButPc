#!/usr/bin/env python3
"""The game files bbport runs: Bloodborne CUSA03173 with the 1.09 update merged in.

Other versions start and then fail inside the game's code (the base game 1.00 faults at guest
offset 0x20348b8): hooks and patches use the addresses of this one executable. The check compares
the loaded executable image, whose hash is the same whatever tool dumped it (the SELF headers
around it differ). BB_SKIP_GAME_CHECK=1 skips it.

An image that differs from 1.09's in a few places only (a 1.09 eboot with byte patches, such as
the 60 FPS packages) starts with a warning: image_109_hashes.txt holds a hash of each 64 KiB of the
1.09 image, and every engine hook and patch checks its own bytes before it writes. More than
PATCHED_CHUNKS pieces differing is another version or a bad dump.
"""
import hashlib
import os
from pathlib import Path

SUPPORTED_TITLE = 'CUSA03173'
SUPPORTED_VERSION = '01.09'
SUPPORTED_IMAGE = '071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a'
CHUNK = 1 << 16
PATCHED_CHUNKS = 32


def differing_chunks(image):
    """How many 64 KiB pieces of `image` differ from the 1.09 image; None when its size differs or
    the list is missing."""
    try:
        lines = (Path(__file__).with_name('image_109_hashes.txt')).read_text(encoding='utf-8').split()
    except OSError:
        return None
    expected = [line for line in lines if len(line) == 16]
    if (len(image) + CHUNK - 1) // CHUNK != len(expected):
        return None
    return sum(hashlib.sha256(image[i * CHUNK:(i + 1) * CHUNK]).hexdigest()[:16] != want
               for i, want in enumerate(expected))


def image_sha256(game):
    """The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses)."""
    from prepare import parse_self, span
    elf, header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


def problem(game, image_hash=None, image=None):
    """None for the supported game, else (kind, title, version): kind is 'missing_update' (base
    game or an older update), 'wrong_eboot' (param.sfo says 1.09, eboot.bin is another version),
    'other_title' (another edition or region) or 'unreadable'."""
    if os.environ.get('BB_SKIP_GAME_CHECK') == '1':
        return None
    from prepare import sfo
    try:
        info = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        info = {}
    title, version = info.get('TITLE_ID', '?'), info.get('APP_VER', '?')
    try:
        if (image_hash or image_sha256(game)) == SUPPORTED_IMAGE:
            return None
    except (OSError, ValueError, IndexError, StopIteration, KeyError):
        return 'unreadable', title, version
    if image is not None and (n := differing_chunks(image)) is not None and n <= PATCHED_CHUNKS:
        print(f'Game check: eboot.bin is 1.09 with {n} of {(len(image) + CHUNK - 1) // CHUNK} '
              '64 KiB pieces changed (a patched eboot, e.g. a 60 FPS package); starting anyway. '
              'If the game crashes, use the clean eboot.bin from the dumped 1.09 update.')
        return None
    if title != SUPPORTED_TITLE:
        return 'other_title', title, version
    if version != SUPPORTED_VERSION:
        return 'missing_update', title, version
    return 'wrong_eboot', title, version


def explain(kind, title, version):
    """What is wrong and what to do, for the log (English)."""
    found = f'Found {title} version {version} (sce_sys/param.sfo).'
    return {
        'missing_update': f'{found} Lantern Engine needs the 1.09 update merged into the game folder: copy '
                          'everything from the dumped 1.09 update (e.g. CUSA03173-patch) into the '
                          'game folder, replacing files (eboot.bin and sce_sys too).',
        'wrong_eboot': f'{found} param.sfo is from 1.09 but eboot.bin is not: copy eboot.bin from '
                       'the dumped 1.09 update into the game folder, replacing the old one.',
        'other_title': f'{found} Only Bloodborne {SUPPORTED_TITLE} with update 1.09 is supported '
                       'for now; other editions and regions have a different executable.',
        'unreadable': f'{found} eboot.bin could not be read as a decrypted PS4 executable: dump '
                      'the game and the 1.09 update again.',
    }[kind]
