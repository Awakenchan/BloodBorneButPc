// SPDX-License-Identifier: GPL-3.0-or-later
#include "shared_camera_settings.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){
 using namespace SharedCamera;
 // The native live-record pointer and a stack-local Defaults copy coexist.
 constexpr std::uint64_t slide=0x100000,manager=0x200000,live=0x300000,defaults=0x400000;
 auto read=[](std::uint64_t address,void* out,std::size_t n){
  std::uint64_t v=0;
  if(address==slide+0x553b130)v=manager;
  else if(address==manager+0x48)v=live;
  else return false;
  if(n!=sizeof v)return false;std::memcpy(out,&v,n);return true;
 };
 assert(IsLiveSource(live,slide,read));
 assert(!IsLiveSource(defaults,slide,read));assert(!IsLiveSource(0,slide,read));
 assert(!IsLiveSource(live,slide,[](auto,auto,auto){return false;}));
 for(int sensitivity=0;sensitivity<=10;++sensitivity)for(int flags=0;flags<8;++flags){
  std::array<std::uint8_t,16> compact{},serialized{};
  compact[0]=serialized[0]=sensitivity;
  compact[9]=serialized[10]=flags&1?0:1;
  compact[10]=serialized[11]=flags&2?0:1;
  compact[12]=serialized[13]=flags&4?1:0;
  Values menu{},game{};assert(DecodeMenu(compact.data(),menu)&&DecodeGame(serialized.data(),game));
  assert(menu.sensitivity==game.sensitivity&&menu.invert_x==game.invert_x&&menu.invert_y==game.invert_y&&menu.auto_adjust==game.auto_adjust);
 }
 RecordChanges dialog;Values no_adjust{5,false,false,false};
 assert(dialog.Observe(no_adjust)==All);assert(dialog.Observe(no_adjust)==0);
 // Simulate a later live record while the closed parent still exists. Its
 // unchanged snapshot cannot reapply stale settings on another menu's tick.
 assert(dialog.Observe(no_adjust)==0);
 Values edited=no_adjust;edited.sensitivity=6;assert(dialog.Observe(edited)==Sensitivity);
 edited.invert_x=true;assert(dialog.Observe(edited)==InvertX);
 edited.invert_y=true;assert(dialog.Observe(edited)==InvertY);
 edited.auto_adjust=true;assert(dialog.Observe(edited)==AutoAdjust);
 edited=no_adjust;assert(dialog.Observe(edited)==All); // Real Defaults action.
 assert(dialog.Observe(edited)==0);
 RecordChanges live_record;
 assert(live_record.Observe(no_adjust)==All); // A saved No beats stale host Yes.
 assert(live_record.Observe(no_adjust)==0); // No replay during a menu edit.
 Values loaded{7,true,false,true};
 assert(live_record.Observe(loaded)==(Sensitivity|InvertX|AutoAdjust));
 assert(dialog.Observe(no_adjust)==0); // Retained parent cannot undo this load.
 std::puts("PASS: factory-record copies rejected; 88 native camera layouts agree; stale menu snapshots cannot overwrite live settings; each real edit and reset publishes its changed fields.");
}
