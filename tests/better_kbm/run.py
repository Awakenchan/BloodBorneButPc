#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test production input/pointer sources against mocked OS/guest services.
--sdk also builds actual native asset serialization and the controller queue.
--game optionally verifies the guest signatures using a local plaintext 1.09 ELF/SELF.
"""
from pathlib import Path
import argparse,shutil,subprocess,tempfile,os,sys,json

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--engine',type=Path,required=True);ap.add_argument('--cxx',default=os.environ.get('CXX','g++'));ap.add_argument('--cc',default=os.environ.get('CC','gcc'));ap.add_argument('--sdk',action='store_true');ap.add_argument('--sdl-include',type=Path);ap.add_argument('--game',type=Path);a=ap.parse_args();engine=a.engine.resolve();here=Path(__file__).resolve().parent
 def run(cmd,**kw):subprocess.run(cmd,check=True,**kw)
 with tempfile.TemporaryDirectory(prefix='better-kbm-') as temp:
  root=Path(temp);inp=root/'gpu/shim/input';eng=root/'gpu/shim/engine';inp.mkdir(parents=True);eng.mkdir()
  for n in ['input.cpp','input.h','compat.h','bindings.cpp','bindings.h','chords.h','pc_controls.cpp','pc_controls.h','ini_bindings.h','mouse_camera_step.h','sprint_turn.h','wheel_buttons.h','menu_pointer.cpp','menu_keyboard_policy.h']:shutil.copy2(engine/'gpu/shim/input'/n,inp/n)
  for n in ['bbport_settings.h','bbport_overlay.h','bbport_platform.h']:shutil.copy2(here/'fixture'/n,root/'gpu/shim'/n)
  p=root/'gpu/shim/bbport_settings.h';s=p.read_text().replace('bool pc_controls=true','bool keyboard_mouse=true,ds3_profile=true,ingame_tweak_menus=true;int button_prompts=0;\n bool pc_controls=true');s+='\nnamespace BbSettings {enum{PromptsKeyboard=1,PromptsController=2};void ApplyDs3Profile();void SelectKeyboardMouse(bool);void RestoreControllerDefaults();}\n';p.write_text(s)
  for n in ['engine_hooks.h','option_menu.h']:shutil.copy2(here/'fixture'/n,eng/n)
  shutil.copy2(engine/'gpu/shim/engine/item_cycle.h',eng/'item_cycle.h');(root/'SDL3').mkdir();shutil.copy2(here/'fixture/SDL.h',root/'SDL3/SDL.h')
  for suite in ['input','pointer']:
   shutil.copy2(here/f'test_{suite}.cpp',inp/f'test_{suite}.cpp');binary=root/(suite+('.exe' if os.name=='nt' else ''))
   cmd=[a.cxx,'-std=c++23','-O0','-I'+str(root),str(inp/f'test_{suite}.cpp')]
   if suite=='input':cmd+=[str(inp/'bindings.cpp'),str(inp/'pc_controls.cpp')]
   run(cmd+['-pthread','-o',str(binary)]);run([str(binary)])
  sprint=root/('sprint-turn.exe' if os.name=='nt' else 'sprint-turn')
  run([a.cxx,'-std=c++23','-O2','-I'+str(engine/'gpu/shim/input'),str(here/'test_sprint_turn.cpp'),'-o',str(sprint)])
  run([str(sprint)])
  wheel=root/('wheel-buttons.exe' if os.name=='nt' else 'wheel-buttons')
  run([a.cxx,'-std=c++23','-O2','-I'+str(engine/'gpu/shim/input'),str(here/'test_wheel_buttons.cpp'),'-o',str(wheel)])
  run([str(wheel)])
  language=root/('game-language.exe' if os.name=='nt' else 'game-language')
  run([a.cxx,'-std=c++23','-O2','-I'+str(engine/'gpu/shim/engine'),str(here/'test_game_language.cpp'),'-o',str(language)])
  run([str(language)])
  banks=root/('voice-banks.exe' if os.name=='nt' else 'voice-banks')
  run([a.cc,'-std=c11','-O2','-I'+str(engine/'src'),str(here/'test_voice_banks.c'),'-o',str(banks)])
  run([str(banks)])
  voice_source=root/'test_voice_files.c'
  voice_source.write_text((here/'test_voice_files.c').read_text().replace('"../../better-engine/src/runtime_file.c"',json.dumps((engine/'src/runtime_file.c').as_posix())))
  voice_files=root/('voice-files.exe' if os.name=='nt' else 'voice-files')
  run([a.cc,'-std=gnu11','-O0','-ffunction-sections','-fdata-sections','-I'+str(engine/'include'),str(voice_source),'-Wl,--gc-sections','-pthread','-o',str(voice_files)])
  voice_data=root/'voice-data';voice_data.mkdir();run([str(voice_files),str(voice_data)])
  settings=root/('movement-settings.exe' if os.name=='nt' else 'movement-settings')
  run([a.cxx,'-std=c++23','-O2','-I'+str(engine/'gpu/shim'),str(here/'test_movement_settings.cpp'),
       str(engine/'gpu/shim/bbport_settings.cpp'),str(engine/'gpu/shim/bbport_menu_i18n.cpp'),'-o',str(settings)])
  run([str(settings)])
  camera=root/('camera-stub.exe' if os.name=='nt' else 'camera-stub')
  run([a.cxx,'-std=c++23','-O2','-mavx','-I'+str(engine/'gpu/shim/input'),str(here/'test_camera_stub.cpp'),str(engine/'gpu/shim/input/mouse_camera_stubs.cpp'),'-o',str(camera)])
  run([str(camera)])
  shared=root/('shared-camera.exe' if os.name=='nt' else 'shared-camera')
  run([a.cxx,'-std=c++23','-O2','-I'+str(engine/'gpu/shim/engine'),str(here/'test_shared_camera.cpp'),'-o',str(shared)])
  run([str(shared)])
  run([sys.executable,str(here/'test_asset_cache.py'),str(engine)])
  if a.sdk:
   # Compile the real serialization, not a duplicate of its layout calculations.
   (root/'pc_option_messages.inc').write_text('R"tsv('+(engine/'tools/engine/pc_option_messages.tsv').read_text()+')tsv"\n')
   for name,original in [('assets','test_assets.cpp'),('pad','test_pad.c'),('quit','test_quit.c')]:
    source=root/original;s=(here/original).read_text()
    target=engine/('tools/engine/menu_assets.cpp' if name=='assets' else 'src/runtime_services.c' if name=='quit' else 'src/runtime_pad.c')
    s=s.replace('"../../better-engine/'+('tools/engine/menu_assets.cpp' if name=='assets' else 'src/runtime_services.c' if name=='quit' else 'src/runtime_pad.c')+'"',json.dumps(target.as_posix()))
    source.write_text(s);binary=root/(name+('.exe' if os.name=='nt' else ''))
    if name=='assets':
     files=[str(engine/'tools/engine'/f) for f in ['esd.cpp','rebirth_script.cpp','sha256.cpp']]
     run([a.cxx,'-std=c++20','-O2','-I'+str(root),str(source),*files,'-lz',*(['-lshell32'] if sys.platform=='win32' or os.environ.get('MSYSTEM') else []),'-o',str(binary)])
    else:
     flags=['-I'+str(a.sdl_include.resolve())] if a.sdl_include else subprocess.check_output(['pkg-config','--cflags','sdl3'],text=True).split()
     run([a.cc,'-std=gnu11','-O0','-ffunction-sections','-fdata-sections',*flags,'-I'+str(engine),'-I'+str(engine/'include'),'-I'+str(engine/'gpu'),str(source),'-Wl,--gc-sections','-pthread','-lm','-o',str(binary)])
    run([str(binary)])
   env=os.environ.copy();env['BB_KBM_TEST_ENGINE']=str(engine)
   # Compile the standalone converter used by the Python/DCX integration test.
   converter=root/('converter.exe' if os.name=='nt' else 'converter')
   run([a.cxx,'-std=c++20','-O2','-I'+str(root),str(engine/'tools/engine/menu_assets.cpp'),*files,'-lz',*(['-lshell32'] if sys.platform=='win32' or os.environ.get('MSYSTEM') else []),'-o',str(converter)])
   env['BB_KBM_TEST_CONVERTER']=str(converter);run([sys.executable,str(here/'test_localization.py')],env=env)
  if a.game:run([sys.executable,str(here/'test_vanilla_game_layout.py'),str(a.game.resolve())])
if __name__=='__main__':main()
