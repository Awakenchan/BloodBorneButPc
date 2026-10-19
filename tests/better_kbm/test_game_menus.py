# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional checks against a player's own menu movies; no game assets shipped."""
import argparse,struct,sys,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--engine',type=Path,required=True);p.add_argument('--converter',type=Path,required=True);p.add_argument('--game-dir',type=Path,required=True);a=p.parse_args()
sys.path.insert(0,str(a.engine.resolve()/'scripts'))
import quit_menu as q
import menu_layout as layout
import pc_localization as loc
import engine_assets as assets

def section(data,name):
 sprites=q.swf.definitions(data)[1];root=q.placements_in(data,q.swf.stage_start(data),len(data));return q.placements_in(data,*sprites[root[name][3]])

def fmg(data,group):
 raw=assets.dcx_unpack(data)
 for _,off,size,mid in q.bnd4_files(raw):
  if mid==group:return loc.fmg_read(raw[off:off+size])
 raise AssertionError(group)

with tempfile.TemporaryDirectory() as t:
 t=Path(t);original=(a.game_dir/'dvdroot_ps4/menu/optionsetting.gfx').read_bytes();movie=t/'options.gfx'
 subprocess.run([str(a.converter.resolve()),'movie',str(a.game_dir/'dvdroot_ps4/menu/optionsetting.gfx'),str(movie)],check=True)
 converted=movie.read_bytes();grown=layout.expand_option_panels(converted);out=q.taller_list(grown) or grown
 assert len(out)==struct.unpack_from('<I',out,4)[0]
 controls=section(out,'ControllSetting');graphics=section(out,'PCGraphics')
 assert controls[3][3]!=graphics[3][3] and controls[3][3]!=section(original,'ControllSetting')[3][3]
 panel_ids=[]
 for name,growth in [('ControllSetting',2400),('PCGraphics',1600),('PCKeys',2400),('PCEffects',-1100),('PCColour',-2200),('PCCamera',-3300)]:
  panel=section(out,name);old=section(converted,name);panel_ids.append(panel[3][3])
  sprites=q.swf.definitions(out)[1];frame=q.placements_in(out,*sprites[panel[3][3]])
  oldsprites=q.swf.definitions(converted)[1];oldframe=q.placements_in(converted,*oldsprites[old[3][3]])
  assert panel[3][3]!=old[3][3]
  assert frame[25][4][2][1]-oldframe[25][4][2][1]==growth
  def bounds(item):
   low,high=q.swf_shape_y(out,item[3]);matrix=item[4];sy=(matrix[0] or (65536,65536))[1]/65536
   return sorted((matrix[2][1]+low*sy,matrix[2][1]+high*sy))
  # Replacing the private frame must preserve all 19 opening/closing
  # animation frames and must never scale the text/widget placements.
  def matrices(blob,timeline):
   rows=[]
   for code,_,body,_ in q.swf.tags(blob,*timeline):
    if code in (26,70):
     cid,span,_=q.swf.place_fields(blob,body,code)
     if span:rows.append((q.depth_of(blob,body,code),q.swf.read_matrix(blob,span[0])[0]))
   return rows
  root_old=q.placements_in(converted,q.swf.stage_start(converted),len(converted))
  root_new=q.placements_in(out,q.swf.stage_start(out),len(out))
  old_rows=matrices(converted,oldsprites[root_old[name][3]])
  new_rows=matrices(out,sprites[root_new[name][3]])
  assert len(old_rows)==len(new_rows)
  status_depth=q.depth_of(out,panel['StatusBar'][1],panel['StatusBar'][0])
  for (depth,before),(new_depth,after) in zip(old_rows,new_rows):
   if name=='PCGraphics' and depth==q.depth_of(converted,old['Item_4_0'][1],old['Item_4_0'][0]):
    assert new_depth==q.depth_of(out,panel['Item_4_0'][1],panel['Item_4_0'][0])
   else:assert depth==new_depth
   if depth==status_depth:
    assert before[:2]==after[:2] and before[2][0]==after[2][0] and after[2][1]-before[2][1]==growth
   else:assert before==after,(name,depth)
  left,right=bounds(frame[11]),bounds(frame[14])
  assert max(abs(x-y) for x,y in zip(left,right))<0.1,(name,left,right)
  assert abs(left[0]-27)<0.1 and abs(left[1]-(7215+growth))<0.1
  assert (frame[11][4][0] or (65536,65536))[1]>0 and frame[14][4][0][1]<0
  if name in ('PCEffects','PCColour','PCCamera'):
   default_index={'PCEffects':5,'PCColour':4,'PCCamera':3}[name]
   default=panel[f'Item_{default_index}_0'];button=q.placements_in(out,*sprites[default[3]])
   label_y=default[4][2][1]+button['Text_0'][4][2][1]
   divider_y=panel[3][4][2][1]+frame[25][4][2][1]
   lower_y=panel[3][4][2][1]+frame[33][4][2][1]
   assert divider_y+500<label_y<lower_y-500,(name,divider_y,label_y,lower_y)
 assert len(set(panel_ids))==6
 image=section(out,'PCColour');camera=section(out,'PCCamera')
 assert len([n for n in image if isinstance(n,str) and n.startswith('Item_')])==5
 assert len([n for n in camera if isinstance(n,str) and n.startswith('Item_')])==4
 # Environment is once again byte-for-byte the original native section.
 original_sprites=q.swf.definitions(original)[1];out_sprites=q.swf.definitions(out)[1]
 original_root=q.placements_in(original,q.swf.stage_start(original),len(original));out_root=q.placements_in(out,q.swf.stage_start(out),len(out))
 lo,hi=original_sprites[original_root['EnvironmentSetting'][3]];nl,nh=out_sprites[out_root['EnvironmentSetting'][3]]
 assert original[lo:hi]==out[nl:nh]
 keys=section(out,'PCKeys')
 for i in range(1,7):assert keys[f'Item_{i}_0'][4][2][1]-keys[f'Item_{i-1}_0'][4][2][1]==1260
 assert keys['Item_7_0'][4][2][1]==controls['Item_8_0'][4][2][1]
 for i in range(1,7):assert controls[f'Item_{i}_0'][4][2][1]-controls[f'Item_{i-1}_0'][4][2][1]==1260
 assert controls['Item_7_0'][3]==controls['Item_8_0'][3]==section(original,'ControllSetting')['Item_6_0'][3]
 assert controls['Item_7_0'][4][2][1]==controls['Item_8_0'][4][2][1]
 # The resolution popup must cover sibling buttons without changing their geometry.
 resolution=graphics['Item_4_0']
 resolution_depth=q.depth_of(out,resolution[1],resolution[0])
 assert resolution_depth>max(q.depth_of(out,p[1],p[0]) for n,p in graphics.items() if isinstance(n,str) and n!='Item_4_0')
 def child(blob,cid,name):
  return q.placements_in(blob,*q.swf.definitions(blob)[1][cid])[name][3]
 popup=resolution[3];original_popup=section(converted,'PCGraphics')['Item_4_0'][3]
 for name in ('Widgets','DropdownList','ItemList'):
  popup=child(out,popup,name);original_popup=child(converted,original_popup,name)
 backing=child(out,popup,1)
 assert backing in q.swf.definitions(out)[0]
 assert child(converted,original_popup,1) in q.swf.definitions(converted)[1]
 assert q.swf_shape_y(out,backing)==(-114,8412)
 # Solid black RGBA fill, opaque; no reused bitmap alpha.
 for code,_,at,size in q.swf.tags(out,q.swf.stage_start(out),len(out)):
  if code==32 and struct.unpack_from('<H',out,at)[0]==backing:
   r=q.swf.Reader(out,at+2);bits=r.unsigned(5)
   assert tuple(r.signed(bits) for _ in range(4))==(-60,8419,-114,8412)
   assert out[r.end():r.end()+7]==b'\x01\x00\x00\x00\x00\xff\x00'
   break
 else:raise AssertionError('Missing opaque popup shape')
 assert graphics['Item_6_0'][3]==graphics['Item_7_0'][3]==graphics['Item_8_0'][3]
 assert graphics['Item_6_0'][4][2][1]==graphics['Item_7_0'][4][2][1]
 assert graphics['Item_6_0'][4][2][0]<graphics['Item_7_0'][4][2][0]==graphics['Item_8_0'][4][2][0]
 assert graphics['Item_8_0'][4][2][1]-graphics['Item_7_0'][4][2][1]==1600
 assert 'Item_5_0' in section(out,'Top')
 for name in q.TITLE_MOVIES:
  b=(a.game_dir/'dvdroot_ps4/menu'/name).read_bytes();assert q.taller_title(b)
 for lang in ('engus','spaes','rusru','jpnjp','frafr','deude','itait','porbr'):
  source=a.game_dir/'dvdroot_ps4/msg'/lang/'menu.msgbnd.dcx';target=t/'messages.dcx'
  subprocess.run([str(a.converter.resolve()),'messages',str(source),str(target)],check=True)
  localized=assets.dcx_pack(loc.localize_bundle(assets.dcx_unpack(target.read_bytes()),lang))
  localized=q.menu_texts(localized,q.TEXTS[lang]);index=loc.language_index(lang);rows=fmg(localized,200)
  for mid,label in [(110007,'Graphics'),(117020,'Graphics'),(110010,'Effects'),(120020,'Effects'),(110014,'Image'),(125020,'Image'),(126000,'Input')]:assert rows[mid]==loc.TRANSLATIONS[label][index],(lang,mid)
  assert rows[q.TITLE_ID]==rows[q.SYSTEM_ID]==q.TEXTS[lang][0]
  assert fmg(localized,204)[q.DIALOG_ID]==q.TEXTS[lang][3]
 assert loc.TRANSLATIONS['KB & Mouse'][:2]==['KB & Mouse','Tecl. y ratón']
print('PASS: real native menu movies: six independent frames with aligned mirrored textures, expanded keys, compact Effects/Image Defaults, Graphics columns and eight localized bundles.')
