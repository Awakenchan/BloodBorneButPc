# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify cached mirror preparation avoids transformation while checking identities."""
import importlib.util,tempfile,sys,unittest
from pathlib import Path
class MirrorCache(unittest.TestCase):
 def test_checked_cache_skips_transform_and_rejects_changed_source(self):
  engine=Path(sys.argv_engine);spec=importlib.util.spec_from_file_location('cache_test_assets',engine/'scripts/engine_assets.py');assets=importlib.util.module_from_spec(spec);spec.loader.exec_module(assets)
  original=b'original game layout fixture';rewritten=b'verified transformed fixture';calls=[]
  assets.INPUT_SHA=assets.sha(original);assets.OUTPUT_SHA=assets.sha(rewritten)
  assets.verify_executable=lambda path:calls.append('verify')
  def transform(raw):self.assertEqual(raw,original);calls.append('transform');return rewritten
  assets.mirror_layout=transform
  with tempfile.TemporaryDirectory() as d:
   d=Path(d);source=d/'game'/assets.LAYOUT;source.parent.mkdir(parents=True);source.write_bytes(assets.dcx_pack(original))
   target=d/'cache/dream-mirror'/assets.LAYOUT;target.parent.mkdir(parents=True);target.write_bytes(assets.dcx_pack(rewritten))
   self.assertEqual(assets.prepare_mirror(d/'game',d/'executable',d/'cache'),target.resolve());self.assertEqual(calls,['verify'])
   target.write_bytes(b'corrupted');calls.clear();assets.prepare_mirror(d/'game',d/'executable',d/'cache');self.assertEqual(calls,['verify','transform']);self.assertEqual(assets.dcx_unpack(target.read_bytes()),rewritten)
   source.write_bytes(assets.dcx_pack(b'changed layout'));calls.clear()
   with self.assertRaises(ValueError):assets.prepare_mirror(d/'game',d/'executable',d/'cache')
   self.assertEqual(calls,['verify'])
if __name__=='__main__':
 sys.argv_engine=sys.argv.pop(1);unittest.main()
