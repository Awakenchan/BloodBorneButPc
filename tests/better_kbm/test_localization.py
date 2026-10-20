from pathlib import Path
import os,sys,struct,subprocess,tempfile,unittest,re
ENGINE=Path(os.environ['BB_KBM_TEST_ENGINE']);sys.path.insert(0,str(ENGINE/'scripts'))
import pc_localization as loc
import engine_assets as assets
class Localization(unittest.TestCase):
 def bundle(self):
  rows={119020:'Key Bindings',126000:'Play with',126003:'Key Bindings',126004:'Camera options',125000:'Brightness',888:'Original',889:None}
  files=[(30,loc.fmg_write({70000006:'Put on Disguise',70000007:'Keep this text'})),(200,loc.fmg_write(rows)),(201,loc.fmg_write({126000:'Use Controller or Keyboard and mouse'})),(999,b'unchanged')]
  head=bytearray(64);head[:4]=b'BND4';struct.pack_into('<I',head,12,len(files));struct.pack_into('<Q',head,32,36)
  names=b''.join(f'{g}.fmg\0'.encode() for g,_ in files);start=64+36*len(files)+len(names);struct.pack_into('<Q',head,40,start);out=head;blob=b'';nameoff=64+len(files)*36
  for g,data in files:
   out+=struct.pack('<QQQIII',0,len(data),len(data),start+len(blob),g,nameoff);blob+=data;nameoff+=len(f'{g}.fmg')+1
  return bytes(out+names+blob)
 def entries(self,b):return {loc.u32(b,e+28):b[loc.u32(b,e+24):loc.u32(b,e+24)+loc.u64(b,e+8)] for e in range(64,64+loc.u32(b,12)*loc.u64(b,32),loc.u64(b,32))}
 def test_eight_languages_and_author_messages(self):
  for i,lang in enumerate(('engus','spaes','rusru','jpnjp','frafr','deude','itait','porbr')):
   data=self.entries(loc.localize_bundle(self.bundle(),lang));rows=loc.fmg_read(data[200]);self.assertEqual(rows[119020],loc.TRANSLATIONS['Key Bindings'][i]);self.assertEqual(rows[126000],loc.TRANSLATIONS['Input'][i]);self.assertEqual(rows[125000],'Brillo' if i==1 else 'Brightness');self.assertEqual(rows[888],'Original');self.assertIsNone(rows[889]);self.assertEqual(data[999],b'unchanged');self.assertEqual(loc.fmg_read(data[30])[70000006],loc.TRANSLATIONS['Change appearance'][i])
  self.assertEqual(loc.localize_bundle(self.bundle(),'korkr'),self.bundle())
 def test_spanish_image_and_effects(self):
  for lang in ('spaes','spaar'):
   catalog=loc.message_catalog(lang)
   data=self.entries(loc.localize_bundle(self.bundle(),lang))
   self.assertEqual(loc.fmg_read(data[200])[125000],'Brillo')
   for key,label in {117005:'Motion Blur',120000:'Bloom',125000:'Brillo',125001:'Contraste',125002:'Saturación',125003:'Tono'}.items():
    self.assertEqual(catalog[key][0],label)
    self.assertTrue(catalog[key][1])
   self.assertEqual(loc.message_catalog('spaes'),loc.message_catalog('spaar'))
 def test_all_action_labels_and_help(self):
  actions=re.findall(r'\{"(\w+)", "([^"]+)", "([^"]+)"',(ENGINE/'gpu/shim/input/bindings.cpp').read_text());self.assertEqual(len(actions),31)
  for _,label,helptext in actions:
   for text in [label,helptext]:self.assertEqual(len(loc.TRANSLATIONS[text]),8);self.assertTrue(all(loc.TRANSLATIONS[text]))
 def test_dcx_and_unicode(self):
  b=loc.fmg_write({1:'Cámara y ratón',2:None,3:'',4:'変更\nНазначение'});self.assertEqual(assets.dcx_unpack(assets.dcx_pack(b)),b);self.assertEqual(loc.fmg_read(b)[4],'変更\nНазначение')
 def test_native_generator_and_python_localizer(self):
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source.dcx';out=Path(d)/'target.dcx';original=self.entries(self.bundle());original[200]=loc.fmg_write({888:'Original',889:None});original[201]=loc.fmg_write({888:'Original help'});original[204]=loc.fmg_write({40300:'Restore defaults?'});original[78]=loc.fmg_write({1:'Original dialog'});
   files=list(original.items());head=bytearray(64);head[:4]=b'BND4';struct.pack_into('<I',head,12,len(files));struct.pack_into('<Q',head,32,36);names=b''.join(f'{g}.fmg\0'.encode() for g,_ in files);start=64+36*len(files)+len(names);struct.pack_into('<Q',head,40,start);blob=b'';entries=b'';offset=64+len(files)*36
   for g,data in files:entries+=struct.pack('<QQQIII',0,len(data),len(data),start+len(blob),g,offset);blob+=data;offset+=len(f'{g}.fmg')+1
   source.write_bytes(assets.dcx_pack(bytes(head+entries+names+blob)))
   subprocess.run([os.environ['BB_KBM_TEST_CONVERTER'],'messages',str(source),str(out)],check=True)
   f=self.entries(assets.dcx_unpack(out.read_bytes()));self.assertIn(126000,loc.fmg_read(f[200]));self.assertEqual(loc.fmg_read(f[200])[125000],'Brightness')
   for lang in ('spaes','spaar'):
    translated=self.entries(loc.localize_bundle(assets.dcx_unpack(out.read_bytes()),lang))
    labels,helptext=loc.fmg_read(translated[200]),loc.fmg_read(translated[201])
    for key,label in {117005:'Motion Blur',120000:'Bloom',125000:'Brillo',125001:'Contraste',125002:'Saturación',125003:'Tono'}.items():
     self.assertEqual(labels[key],label)
     self.assertEqual(helptext[key],loc.message_catalog(lang)[key][1])
if __name__=='__main__':unittest.main()
