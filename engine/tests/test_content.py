from paths import ROOT
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import content_profile

EXE=ROOT/'out/content-test'

class ProfileTests(unittest.TestCase):
    def test_sfo_parameters_and_explicit_trial_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':13,'USER_DEFINED_PARAM_4':0xffffffff}):
                content_profile.prepare(p,p,'trial')
            self.assertEqual(struct.unpack('<8s5I',(p/'content.bin').read_bytes()),(b'BBCONT01',1,13,0,0,0xffffffff))

    def test_invalid_sfo_parameter_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':'13'}):
                with self.assertRaisesRegex(ValueError,'invalid user-defined'):
                    content_profile.prepare(p,p)

@unittest.skipUnless(EXE.exists(),'run bash build.sh --test first')
class ContentTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=5)

    def test_lifecycle_and_output_boundaries(self):
        r=self.run_case();self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_missing_profile_does_not_report_loaded(self):
        r=self.run_case('--missing');self.assertEqual(r.returncode,21)
        self.assertIn('needs --content-profile',r.stderr)

    def test_dumped_addons_are_reported_installed(self):
        def sfo(entries):
            keys=data=index=b''
            for k,v in entries:
                kb,vb=k.encode()+b'\0',v.encode()+b'\0'
                index+=struct.pack('<HHIII',len(keys),0x0204,len(vb),len(vb),len(data)); keys+=kb; data+=vb
            start=20+len(index)
            return b'\0PSF'+struct.pack('<IIII',0x101,start,start+len(keys),len(entries))+index+keys+data
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            for folder,entries in [('dlc/SPEXPANSIONDLC03',None),
                                   ('dlc/oldhunters',[('CATEGORY','ac'),('CONTENT_ID','UP9000-CUSA00207_00-SPEXPANSIONDLC03')]),
                                   ('dlc/game',[('CATEGORY','gd'),('CONTENT_ID','UP9000-CUSA00207_00-BLOODBORNE0000000')]),
                                   ('dlc/dvdroot_ps4',None),('user/addcont/CUSA00207/SOUNDTRACK01',None)]:
                (p/folder/'sce_sys').mkdir(parents=True)
                if entries: (p/folder/'sce_sys/param.sfo').write_bytes(sfo(entries))
            r=subprocess.run([str(EXE.resolve()),'--addons'],capture_output=True,text=True,timeout=5,
                             env={'BB_DLC_DIR':str(p/'dlc'),'TEST_USER_DIR':str(p/'user')})
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertIn('hits 2',r.stdout)
            self.assertIn('addon SPEXPANSIONDLC03 4',r.stdout)
            self.assertIn('addon SOUNDTRACK01 4',r.stdout)
            r=self.run_case('--addons');self.assertIn('hits 0',r.stdout)

    def test_other_modules_are_counted_independently(self):
        r=self.run_case('--unknown');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
