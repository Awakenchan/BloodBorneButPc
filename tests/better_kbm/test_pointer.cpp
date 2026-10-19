// SPDX-License-Identifier: GPL-3.0-or-later
// Run the ACTUAL pointer/list implementation against synthetic guest objects.
#define TEST_POINTER
#include "menu_pointer.cpp"
#include <cassert>
#include <vector>
#include <cstdarg>
static std::vector<uint8_t> arena(0x5a00000);
static bool mode=true,movement=false,focus=true,controller=false,arrows=false,decides=false;
static float px=10,py=10;
static int delivered_focus=-1,cursor_sets=0,scrolls=0;
static uint64_t base(){return reinterpret_cast<uint64_t>(arena.data());}
static void* at(size_t offset){return arena.data()+offset;}
template<class T>static void put(void* p,size_t o,T v){std::memcpy(static_cast<uint8_t*>(p)+o,&v,sizeof v);}
static uint8_t* component=arena.data()+0x5800000;
static uint64_t makefn=0x410000,boundsfn=0x410100;
static uint8_t* object(int row){return arena.data()+0x5810000+row*0x200;}
static uint8_t* datafor(int row){return arena.data()+0x5820000+row*0x100;}
static void wrapper(void* dst,int row){std::memset(dst,0,0x80);put(dst,0x48,uint32_t(10));put(dst,0x50,reinterpret_cast<uint64_t>(datafor(row)));}
bool test_safe_read(const void* p,void* out,size_t n){auto a=reinterpret_cast<uint64_t>(p);if(a<base()||a-base()>arena.size()||n>arena.size()-(a-base()))return false;std::memcpy(out,p,n);return true;}
int64_t test_guest_call(void* fn,const int64_t* a,size_t){
 const auto off=reinterpret_cast<uint64_t>(fn)-base();
 if(off==makefn){auto packed=*reinterpret_cast<const uint64_t*>(a[2]);wrapper(reinterpret_cast<void*>(a[0]),int(packed>>32));return 0;}
 if(off==kChild){
  const auto* name=reinterpret_cast<const char*>(a[2]);
  if(arrows&&!std::strcmp(name,"Widgets/Slider/LeftArrow")){wrapper(reinterpret_cast<void*>(a[0]),3);return 0;}
  if(arrows&&!std::strcmp(name,"Widgets/Slider/RightArrow")){wrapper(reinterpret_cast<void*>(a[0]),4);return 0;}
  if(std::strcmp(name,"Cursor")){std::memset(reinterpret_cast<void*>(a[0]),0,0x80);return 0;}
  auto* src=reinterpret_cast<uint8_t*>(a[1]);uint64_t data=field<uint64_t>(src,0x50);
  int row=int((data-reinterpret_cast<uint64_t>(datafor(0)))/0x100);wrapper(reinterpret_cast<void*>(a[0]),row);return 0;
 }
 if(off==boundsfn){auto* out=reinterpret_cast<float*>(a[0]);int row=int((a[1]-reinterpret_cast<int64_t>(object(0)))/0x200);if(row>=3){out[0]=row==3?1000:3000;out[1]=0;out[2]=out[0]+200;out[3]=900;}else{out[0]=0;out[1]=row*1000.f;out[2]=4000;out[3]=out[1]+900;}return 0;}
 if(off==kWorldMatrix){float m[8]={1,0,0,0,0,1,0,0};std::memcpy(reinterpret_cast<void*>(a[1]),m,sizeof m);return 0;}
 if(off==kSetCursor){put(reinterpret_cast<void*>(a[0]),kCursor,int32_t(a[1]));cursor_sets++;return 1;}
 if(off==kScrollBy){scrolls++;return 0;}
 if(off==kHideCursors)return 0;
 if(off==kListUpdate){delivered_focus=*reinterpret_cast<const uint8_t*>(a[1]);return 0;}
 assert(false && "Unexpected guest function");return 0;
}
HostSettings host_settings(){return {mode};}InputDevice host_input_device(){return controller?InputDevice::Pad:InputDevice::KeyboardMouse;}
PadState host_pad_state(){PadState p;if(movement)p.ly=0;return p;}
bool host_keyboard_moving(){return movement;}
bool host_mouse_stage_position(float& x,float& y){x=px;y=py;return focus;}
bool host_mouse_relative(){return false;}void host_mouse_describe(char* p,size_t n){std::snprintf(p,n,"test");}
bool host_options_open(){return false;}bool ingame_menu_open(){return false;}bool host_text_entry_open(){return false;}bool debug_menu_open(){return false;}
bool option_menu_click_decides(uint8_t*,int){return decides;}void option_menu_list_update(uint8_t*,bool){}
void host_log(const char*,...){}
int main(){
 // Reproduce the image+guest-VA layout used by the ORIGINAL loader.
 g_slide=base()+0x400000;
 const uint64_t flow=base()+0x5830000;put(at(kMenuFlow),0,flow);
 put(reinterpret_cast<void*>(flow),kFlowStarted,uint8_t(0));put(reinterpret_cast<void*>(flow),kFlowMenu,uint64_t(1));
 put(at(kRegion),0,int32_t(1));std::memcpy(at(kCursorName),"Cursor",7);
 put(component,kCount,int32_t(3));put(component,kCursor,int32_t(0));put(component,kCursorOverride,int32_t(-1));
 put(component,kLineLen,int32_t(1));put(component,kLines,int32_t(3));
 const uint64_t provider=base()+0x5840000,vt=base()+0x5841000;
 put(component,kProvider,provider);put(reinterpret_cast<void*>(provider),0,vt);put(reinterpret_cast<void*>(vt),0x18,base()+makefn);
 const uint64_t objvt=base()+0x5842000;put(reinterpret_cast<void*>(objvt),kGetBounds,base()+boundsfn);
 for(uint64_t off:{makefn,boundsfn,kWorldMatrix,kChild,kSetCursor,kScrollBy,kHideCursors,kListUpdate}){uint8_t pro[]={0x55,0x48,0x89,0xe5};std::memcpy(at(off),pro,4);}
 for(int i=0;i<5;i++){put(object(i),0,objvt);put(object(i),kFlags,uint8_t(kVisible));put(datafor(i),kNative,reinterpret_cast<uint64_t>(object(i)));}
 // Selector ticks share the native queue and respect louder pending sounds.
 auto* sound=static_cast<int32_t*>(at(kMenuSe));
 sound[0]=-1;sound[1]=-1;menu_pointer_selection_sound();
 assert(sound[0]==0&&sound[1]==0);
 assert(field<uint64_t>(static_cast<uint8_t*>(at(kMenuSeLabel)),0)==base()+kCursorMoveLabel);
 sound[0]=3;sound[1]=1;menu_pointer_selection_sound();
 assert(sound[0]==3&&sound[1]==1);
 uint8_t active=1;
 movement=true;pc_list_update(component,&active);assert(delivered_focus==1); // TITLE keeps WASD.
 put(reinterpret_cast<void*>(flow),kFlowStarted,uint8_t(1));
 py=60;pc_list_update(component,&active);assert(delivered_focus==0);assert(cursor_of(component)==1);assert(cursor_sets==1); // WORLD can move AND hover.
 menu_pointer_click();pc_list_update(component,&active);assert(menu_pointer_take_press()==0x4000); // LMB selects actual hit item.
 assert(menu_back_button()==0x2000);put(at(kRegion),0,int32_t(5));assert(menu_back_button()==0x4000); // Region aware back.
 controller=true;pc_list_update(component,&active);assert(delivered_focus==1);controller=false; // Controller keeps list navigation.
 mode=false;pc_list_update(component,&active);assert(delivered_focus==1); // Disabled KBM leaves input intact.
 mode=true;movement=false;pc_list_update(component,&active);assert(delivered_focus==1); // Native pad navigation unaffected.
 put(reinterpret_cast<void*>(flow),kFlowMenu,uint64_t(0));g_focus_ms=now_ms()-35;assert(!menu_pointer_in_menu());
 put(reinterpret_cast<void*>(flow),kFlowStarted,uint8_t(0));assert(menu_pointer_in_menu());
 // Category arrows reach the actual left/right geometry. Action rows retain capture.
 arrows=true;assert(click_button(component,0,0,55,10)==0x80u);
 assert(click_button(component,0,0,155,10)==0x20u);
 decides=true;assert(click_button(component,1,0,55,10)==menu_confirm_button());
 decides=false;arrows=false;
 assert(active==1); // Never mutate caller's focus flag or gameplay pad.
 std::puts("PASS: real hit-test/hover/click implementation; title WASD, world movement isolation, KBM-off and region back.");
}
