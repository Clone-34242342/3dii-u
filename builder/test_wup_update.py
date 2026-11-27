"""Native update identities, selected payloads and fixed-version reinstalls."""
from datetime import datetime, timezone, timedelta
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import wup_update as u
class UpdateTests(unittest.TestCase):
 def test_native_version_lookup_identity(self):
  self.assertEqual(u.native_base_id(0x000500003d7c0545,0x0004000000054500),0x000500001d054500)
  self.assertEqual(u.native_base_id(0x000500001d054500),0x000500001d054500)
  self.assertEqual(u.native_base_id(0x0005000010334455,0x0004000000030800),0x000500001d030800)
  for legacy in (0x000500003d7c0545,0x000500001d7c0545,0x0005000010334455):
   with self.assertRaisesRegex(ValueError,'legacy WUP'):u.native_base_id(legacy)
  for unrelated in (0x000500001018dc00,0x0005001010040100):
   with self.assertRaises(ValueError):u.native_base_id(unrelated,0x0004000000030800)
 def test_native_ids_keep_adjacent_games_distinct(self):
  ids=[u.native_base_id(0x000500003d000000,0x0004000000000000|(unique<<8)) for unique in range(0x10000)]
  self.assertEqual(len(set(ids)),0x10000)
  self.assertTrue(all(t&0xff==0 and ((t&0x0fffffff)|0x10000000)==(t&0xffffffff) for t in ids))
  self.assertEqual(ids[0x308],0x000500001d030800)
  self.assertNotEqual(ids[0x308],ids[0x309])
  for program in (0x0004000001000000,0x0004000000030801,0x0004000e00030800):
   with self.assertRaises(ValueError):u.native_base_id(0x000500003d000308,program)
 def test_fixed_version_ignores_clock_and_build_count(self):
  dates=[datetime(2000,1,1,tzinfo=timezone.utc),datetime(2026,11,27,tzinfo=timezone.utc),datetime(2200,1,1,tzinfo=timezone.utc)]
  for date in dates:
   for _ in range(5):self.assertEqual(u.update_build_info(date)[0],5)
  with self.assertRaises(ValueError):u.update_build_info(dates[0].replace(tzinfo=None))
 def test_binary_update_preserves_shaders_without_recompiling_them(self):
  import json
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);base=root/'base'
   for folder in ('code','meta','content'): (base/folder).mkdir(parents=True)
   (base/'code/app.xml').write_text('<app><title_id>000500001D054500</title_id><title_version>0000</title_version><app_type>80000000</app_type></app>')
   (base/'meta/meta.xml').write_text('<menu><title_id>000500001D054500</title_id><title_version>0</title_version></menu>')
   (base/'code/game.rpx').write_bytes(b'executable')
   sizes={'shaders.gsh':123,'shaders.idx':456,'fallback-shaders.bin':789}
   for name,size in sizes.items(): (base/'content'/name).write_bytes(b'x'*size)
   inventory=root/'build-cache/base.json'
   u.remember_base(inventory,base,0x000500001d054500)
   for name in sizes: (base/'content'/name).unlink()
   with self.assertRaisesRegex(ValueError,'links'):
    u.make_update(base,root/'missing','binary')
   self.assertFalse((root/'missing').exists())
   files=u.base_shaders(inventory,0x000500001d054500)
   u.make_update(base,root/'update','binary',base_shader_files=files)
   links=json.loads((root/'update/.3dii-base-links.json').read_text())['files']
   self.assertEqual(links,sizes)
   self.assertFalse(any((root/'update/content'/name).exists() for name in sizes))
   for bad in (root/'missing.json',inventory):
    with self.assertRaisesRegex(ValueError,'matching base'):
     u.base_shaders(bad,0x000500001d030800)
   for bad in ({'shaders.gsh':1},dict(sizes,**{'shaders.idx':-1}),dict(sizes,**{'../bad':3})):
    with self.assertRaises(ValueError):u.make_update(base,root/'bad','binary',base_shader_files=bad)
 def test_payload_and_native_identity(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);base=root/'base'
   for folder in ('code','content','meta'): (base/folder).mkdir(parents=True)
   (base/'code/app.xml').write_text('<app><title_id>000500001D054500</title_id><title_version>0003</title_version><app_type>80000000</app_type></app>')
   (base/'meta/meta.xml').write_text('<menu><title_id>000500001D054500</title_id><title_version>3</title_version></menu>')
   (base/'code/game.rpx').write_bytes(b'new binary');(base/'code/cos.xml').write_text('<app/>')
   for name in ('romfs.bin','shaders.gsh','shaders.idx','fallback-shaders.bin'): (base/'content'/name).write_bytes(name.encode())
   stamp=datetime(2026,11,27,tzinfo=timezone.utc)
   for parts in u.PARTS:
    out=root/parts;info=u.make_update(base,out,parts,5,stamp)
    self.assertEqual(ET.parse(out/'code/app.xml').findtext('title_id'),'0005000E1D054500')
    self.assertEqual(ET.parse(out/'meta/meta.xml').findtext('title_id'),'000500001D054500')
    self.assertEqual(ET.parse(out/'code/app.xml').findtext('app_type'),'0800001B')
    self.assertEqual(info['version'],5)
    self.assertFalse((out/'content/romfs.bin').exists())
    import json
    links=json.loads((out/'.3dii-base-links.json').read_text())
    self.assertIn('romfs.bin',links['files'])
    self.assertEqual('shaders.gsh' in links['files'],parts=='binary')
    u.make_update(base,out,parts,5,stamp)
    self.assertEqual((out/'content/shaders.gsh').exists(),parts=='both')
   xml=base/'code/app.xml';xml.write_text(xml.read_text().replace('000500001D054500','000500003D7C0545'))
   with self.assertRaisesRegex(ValueError,'legacy WUP'):
    u.make_update(base,root/'legacy','binary',5,stamp)
   self.assertFalse((root/'legacy').exists())
   xml.write_text(xml.read_text().replace('000500003D7C0545','000500001D054500'))
   (base/'content/shaders.idx').unlink()
   with self.assertRaises(ValueError): u.make_update(base,root/'bad','both')
   self.assertFalse((root/'bad').exists())
if __name__=='__main__': unittest.main()
