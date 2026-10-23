from paths import ROOT
import unittest
import struct
import engine_assets
import tempfile
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace


class EngineAssetTests(unittest.TestCase):
    def test_dcx_roundtrip(self):
        raw = b'MSB fixture\0' * 500
        packed = engine_assets.dcx_pack(raw)
        self.assertEqual(len(packed[:0x4c]), 0x4c)
        self.assertEqual(engine_assets.dcx_unpack(packed), raw)

    def test_bad_size_or_truncated_stream_is_rejected(self):
        packed = bytearray(engine_assets.dcx_pack(b'unchanged source' * 20))
        struct.pack_into('>I', packed, 0x1c, 1)
        with self.assertRaises(ValueError):
            engine_assets.dcx_unpack(packed)
        with self.assertRaises(ValueError):
            engine_assets.dcx_unpack(engine_assets.dcx_pack(b'valid')[:-4])

    def test_wrong_layout_does_not_mutate_input(self):
        raw = bytearray(b'not a supported Dream layout')
        before = bytes(raw)
        with self.assertRaisesRegex(ValueError, 'original 1.09'):
            engine_assets.mirror_layout(raw)
        self.assertEqual(raw, before)

    def test_menu_cache_rebuilds_corrupt_output_and_changed_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            movie = game / 'dvdroot_ps4/menu/optionsetting.gfx'
            messages = game / 'dvdroot_ps4/msg/engus/menu.msgbnd.dcx'
            for p in (movie, messages):
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'own dump')
            tool = root / 'tool'
            tool.write_bytes(b'generator version 1')
            calls = []
            def generate(args, **kwargs):
                calls.append(args[1])
                Path(args[3]).write_bytes(Path(args[2]).read_bytes() + b' prepared')
                return SimpleNamespace(returncode=0, stderr='')
            with patch.object(engine_assets, 'verify_executable'), patch.object(engine_assets.subprocess, 'run', generate):
                cache = engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 2)
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 2)
                cached = cache / movie.relative_to(game)
                cached.write_bytes(b'corrupt')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 3)
                self.assertEqual(movie.read_bytes(), b'own dump')
                messages.write_bytes(b'updated language')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 4)
                tool.write_bytes(b'generator version 2')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 6)

    def test_menu_generation_failure_does_not_publish_manifest_or_change_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            for name in ('menu/optionsetting.gfx', 'msg/engus/menu.msgbnd.dcx'):
                p = game / 'dvdroot_ps4' / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'original')
            tool = root / 'tool'
            tool.write_bytes(b'tool')
            with patch.object(engine_assets, 'verify_executable'), patch.object(engine_assets.subprocess, 'run',
                    return_value=SimpleNamespace(returncode=1, stderr='unsupported movie')):
                with self.assertRaisesRegex(ValueError, 'unsupported movie'):
                    engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
            self.assertFalse((root / 'cache/pc-menus/manifest.json').exists())
            self.assertTrue(all(p.read_bytes() == b'original' for p in game.rglob('*') if p.is_file()))

    @staticmethod
    def binder(files):
        """A BND4 laid out as the game's effect binders: 0x24-byte entries, UTF-16 names, data
        16-byte aligned after the names."""
        count = len(files)
        names = b''.join(('N:\\sfx\\effect\\' + n).encode('utf-16-le') + b'\0\0' for n, _ in files)
        headers_end = 0x40 + 0x24 * count + len(names)
        header = bytearray(b'BND4' + bytes(4) + b'\0\0\x01\0' + struct.pack('<iq', count, 0x40) + b'07D7R6\0\0'
                           + struct.pack('<qq', 0x24, headers_end) + b'\x01\x74\0\0' + bytes(12))
        entries, data, at, name_at = bytearray(), bytearray(), (headers_end + 15) & ~15, 0x40 + 0x24 * count
        for i, (name, body) in enumerate(files):
            entries += struct.pack('<B3xiqqIii', 0x40, -1, len(body), len(body), at + len(data), i, name_at)
            name_at += len(('N:\\sfx\\effect\\' + name).encode('utf-16-le')) + 2
            data += body + bytes(-len(body) % 16)
        out = bytes(header) + bytes(entries) + names
        return out + bytes(-len(out) % 16) + bytes(data)

    def test_effect_binder_entry_is_replaced_in_place_of_its_data(self):
        bnd = self.binder([('f000000001.fxr', b'first'), ('f001251720.fxr', b'trail effect'), ('f000000003.fxr', b'third')])
        out = engine_assets.replace_entry(bnd, 'f001251720.fxr', b'the replacement effect')
        self.assertEqual(out[:len(bnd)], bnd[:0x40 + 0x24] + out[0x40 + 0x24:0x40 + 0x48] + bnd[0x40 + 0x48:])
        self.assertEqual(len(out) % 16, 6)
        self.assertEqual(engine_assets.binder_file(out, 'f001251720.fxr'), b'the replacement effect')
        self.assertEqual(engine_assets.binder_file(out, 'f000000001.fxr'), b'first')
        self.assertEqual(engine_assets.binder_file(out, 'f000000003.fxr'), b'third')
        with self.assertRaisesRegex(ValueError, 'not in the effect binder once'):
            engine_assets.replace_entry(bnd, 'f009999999.fxr', b'x')
        with self.assertRaisesRegex(ValueError, 'layout'):
            engine_assets.binder_entries(b'BND3' + bnd[4:])

    def test_effect_fix_leaves_a_modded_binder_alone(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binder = root / 'game' / engine_assets.EFFECT_FIXES[0][0]
            binder.parent.mkdir(parents=True)
            binder.write_bytes(engine_assets.dcx_pack(self.binder([('f001251720.fxr', b'a mod\'s effect')])))
            lines = []
            self.assertEqual(engine_assets.prepare_effect_fixes(root / 'game', root / 'cache', log=lines.append), [])
            self.assertIn('not the 1.09 file', lines[0])
            self.assertFalse((root / 'cache').exists())


if __name__ == '__main__':
    unittest.main()
