// SPDX-License-Identifier: GPL-3.0-or-later
// The actual input bridge and binding implementation; SDL/renderer services are mocked.
#include "input.cpp"
#include <cassert>
#include <iostream>
static bool test_menu=false,test_front=false,test_navigating=false,test_overlay=false,test_capture=false;
namespace BbOverlay { bool MenuOpen(){return test_overlay;} bool CapturesInput(){return test_overlay;} }
namespace BbEngine::Options {bool ClickDecides(uint8_t*,int){return false;} void ListUpdate(uint8_t*,bool){} }
void mouse_camera_install(ElfImage*){} bool mouse_camera_installed(){return true;}
static bool published_mouse=false,published_hold=false,flick_enabled=false;
void mouse_camera_publish(int,bool,bool,bool hold,bool enabled){published_mouse=enabled;published_hold=hold;}
bool mouse_camera_flick(float& x,float& y){x=1;y=0;return flick_enabled;}
void menu_pointer_install(ElfImage*){} bool menu_pointer_in_menu(){return test_menu;}
bool menu_pointer_navigating(){return test_navigating;} bool menu_pointer_front_end(){return test_front;}
void menu_pointer_click(){test_capture=true;} uint32_t menu_pointer_take_press(){return test_capture?(test_capture=false,0x4000):0;}
uint32_t menu_confirm_button(){return 0x4000;} uint32_t menu_back_button(){return 0x2000;}
namespace BbEngine::ItemCycle {bool Install(uint8_t*,size_t){return true;}}
static bool reverse_request=false;
void native_item_cycle_request(bool reverse){reverse_request=reverse;}void native_item_cycle_cancel(){reverse_request=false;}
void menu_pointer_scroll(int){}
extern "C" uint64_t runtime_pad_bindings_serial(){return 0;}
extern "C" int runtime_pad_export_binding(int,char*,size_t,char*,size_t){return 0;}
extern "C" int runtime_pad_button(uint32_t,int){return -1;}
extern "C" void runtime_pad_request_binding(uint32_t,int,int){}
static bool keys[512]{};
static void key(const char* n,bool down=true){auto sc=SDL_GetScancodeFromName(n);host_bindings_key_event(sc,down);keys[sc]=down;}
static BbPcPad sample(bool gamepad=false,BbPcPad p={}){bbgpu_pc_input(&p,keys,512,gamepad);return p;}
static void clear(){for(int i=0;i<512;i++)if(keys[i])host_bindings_key_event(i,false);std::fill_n(keys,512,false);mouse_buttons=mouse_edges=0;wheel=0;}
int main(){
 active=true;host_bindings_load_defaults();
 assert(host_binding_ini_value(kBindDpadUp)=="T");
 // Explicit saved/custom bindings still override defaults.
 host_bindings_parse("dpad_up","Y");assert(host_binding_ini_value(kBindDpadUp)=="Y");
 BbSettings::ApplyDs3Profile();
 auto& settings=BbSettings::Get();settings.mouse_menu=true;
 key("W");key("Space");auto p=sample();assert(p.ly==0 && (p.buttons&0x2000));
 key("A");p=sample();assert(p.lx<=127 && p.ly==0 && (p.buttons&0x2000));
 test_menu=test_navigating=true;
 // WASD moves the player without scrolling or backing out of the inventory.
 p=sample();assert(p.lx==0&&p.ly==0&&!(p.buttons&0xf0)&&!(p.buttons&0x2000));assert(host_keyboard_moving());
 key("Up");p=sample();assert((p.buttons&0x10)&&p.lx==0&&p.ly==0);key("Up",false);
 // Controller mode preserves physical input and keyboard fallback.
 BbSettings::SelectKeyboardMouse(false);
 BbPcPad physical{};physical.buttons=0x2000;physical.lx=200;physical.ly=127;
 clear();p=sample(true,physical);assert((p.buttons&0x2000)&&p.lx==200&&!host_keyboard_moving());
 BbSettings::SelectKeyboardMouse(true);
 key("W");test_front=true;p=sample();assert((p.buttons&0x10)&&p.ly==127&&!host_keyboard_moving());
 test_front=false;settings.mouse_menu=false;p=sample();assert((p.buttons&0x10)&&p.ly==127);
 settings.mouse_menu=true;clear();key("Backspace");p=sample();assert(p.buttons&0x2000);
 clear();mouse_edges=SDL_BUTTON_RMASK;p=sample();assert(p.buttons&0x2000);
 clear();mouse_edges=1;p=sample();assert(p.buttons&0x4000);
 clear();key("Escape");p=sample();assert((p.buttons&8)&&!(p.buttons&0x2000));
 test_menu=test_navigating=false;
 clear();key("E");p=sample();assert((p.buttons&0x4000)&&!(p.buttons&0x8000));
 clear();key("R");p=sample();assert((p.buttons&0x8000)&&!(p.buttons&0x4000));
 clear();key("C");p=sample();assert(p.buttons&0x1000);
 clear();mouse_buttons=1;p=sample();assert((p.buttons&0x800)&&!(p.buttons&0x200));
 key("Left Shift");p=sample();assert((p.buttons&0x200)&&p.r2==255&&!(p.buttons&0x800));
 key("Left Shift",false);p=sample();assert(!(p.buttons&0xa00));
 clear();sample();mouse_buttons=SDL_BUTTON_RMASK;p=sample();assert((p.buttons&0x100)&&p.l2==255&&!(p.buttons&0x400));
 clear();sample();key("Left Ctrl");p=sample();assert((p.buttons&0x100)&&!(p.buttons&2));
 clear();wheel=-1;p=sample();assert(p.buttons&0x40);
 clear();sample();wheel=1;p=sample();assert((p.buttons&0x40)&&reverse_request);
 clear();sample();reverse_request=false;key("Left Shift");wheel=1;p=sample();assert((p.buttons&0x20)&&!(p.buttons&0x40)&&!reverse_request);
 clear();sample();key("Left Shift");wheel=-1;p=sample();assert((p.buttons&0x80)&&!(p.buttons&0x40)&&!reverse_request);
 clear();sample();key("Down");p=sample();assert(!(p.buttons&0x40));
 clear();sample();
 // Resolve real SDL wheel events while Shift is still held, before the next pad read.
 clear();sample();weapon_wheel.Reset();reverse_request=false;
 SDL_Event shift_event{};shift_event.type=SDL_EVENT_KEY_DOWN;shift_event.key.scancode=SDL_GetScancodeFromName("Left Shift");
 BbInput::Event(shift_event,false);
 SDL_Event wheel_event{};wheel_event.type=SDL_EVENT_MOUSE_WHEEL;wheel_event.wheel.y=1;
 BbInput::Event(wheel_event,false);shift_event.type=SDL_EVENT_KEY_UP;BbInput::Event(shift_event,false);
 p=sample();assert((p.buttons&0x20)&&!(p.buttons&0xc0)&&!reverse_request);
 for(int poll=0;poll<20;++poll){p=sample();assert((p.buttons&0x20)&&!(p.buttons&0xc0));}
 weapon_wheel.Reset();shift_event.type=SDL_EVENT_KEY_DOWN;BbInput::Event(shift_event,false);
 wheel_event.wheel.y=-1;BbInput::Event(wheel_event,false);
 shift_event.type=SDL_EVENT_KEY_UP;BbInput::Event(shift_event,false);
 p=sample();assert((p.buttons&0x80)&&!(p.buttons&0x60));
 test_menu=test_navigating=true;p=sample();assert(!(p.buttons&0xa0)&&weapon_wheel.pending.empty());
 test_menu=test_navigating=false;clear();sample();
 // The DS3 profile uses T for Blood Bullets without binding the title arrows.
 clear();key("T");p=sample();assert(p.buttons&0x10u);
 clear();sample();
 // Chord capture accumulates until all constituent inputs have been released.
 host_bind_capture_begin(kBindFirearm);clear();sample();
 host_bind_capture_key(SDL_GetScancodeFromName("Left Shift"));key("Left Shift");host_bind_capture_mouse(3);mouse_buttons=SDL_BUTTON_RMASK;sample();
 assert(host_bind_capturing()==kBindFirearm);key("Left Shift",false);sample();assert(host_bind_capturing()==kBindFirearm);
 mouse_buttons=0;sample();assert(host_bind_capturing()==-1);assert(host_bind_capture_take_done()==kBindFirearm);
 assert(host_binding_ini_value(kBindFirearm).find(" + ")!=std::string::npos);
 clear();sample();key("Left Shift");mouse_buttons=SDL_BUTTON_RMASK;p=sample();assert(p.buttons&0x100);
 clear();sample();mouse_buttons=SDL_BUTTON_RMASK;p=sample();assert(!(p.buttons&0x100));
 clear();sample();BbSettings::SelectKeyboardMouse(false);key("E");p=sample();assert((p.buttons&0x4000)&&settings.pc_controls&&!settings.mouse_camera);
 BbSettings::SelectKeyboardMouse(true);assert(settings.mouse_camera);
 // Rollover reversals must keep sprint held and choose the latest direction.
 clear();test_menu=test_navigating=false;key("Space");key("A");p=sample();assert(p.lx==0);
 key("D");p=sample();assert(p.lx==254&&(p.buttons&0x2000));
 // Repeated older key events must not steal direction back.
 key("A");p=sample();assert(p.lx==254);
 key("D",false);p=sample();assert(p.lx==0&&(p.buttons&0x2000));
 clear();sample();key("Space");key("W");p=sample();assert(p.ly==0);
 key("S");p=sample();assert(p.ly==254&&(p.buttons&0x2000));
 // Both edges between pad reads preserve SDL event order.
 clear();sample();key("D");key("A");p=sample();assert(p.lx==0);
 clear();sample();key("A");key("D");p=sample();assert(p.lx==254);
 // Adding a diagonal keeps sprint; there is no direction interpolation.
 clear();sample();key("Space");key("W");sample();key("A");p=sample();assert(p.lx<=127&&p.ly==0&&(p.buttons&0x2000));
 // Controller axes have priority over synthetic mouse target-switch input.
 clear();sample();device=InputDevice::Pad;BbSettings::SelectKeyboardMouse(false);flick_enabled=true;
 for(int x=0;x<256;++x)for(int y=0;y<256;++y){
  BbPcPad pad{};pad.rx=x;pad.ry=y;pad.lx=pad.ly=128;
  p=sample(true,pad);assert(p.rx==x&&p.ry==y&&!published_mouse&&!published_hold);
 }
 BbSettings::SelectKeyboardMouse(true);device=InputDevice::KeyboardMouse;flick_enabled=false;
 BbPcPad pad{};pad.rx=190;pad.ry=170;
 pad.buttons=0xffff;pad.lx=0;pad.ly=254;pad.l2=pad.r2=255;pad.touch_count=1;
 p=sample(true,pad);assert(p.rx==127&&p.ry==127&&p.lx==127&&p.ly==127&&!p.buttons&&!p.l2&&!p.r2&&!p.touch_count&&published_mouse);
 flick_enabled=false;clear();sample();assert(published_mouse);
 pad.rx=140;pad.ry=128;p=sample(true,pad);assert(published_mouse&&p.rx==127&&p.ry==127);
 SDL_Event pad_event{};pad_event.type=SDL_EVENT_GAMEPAD_AXIS_MOTION;pad_event.gaxis.value=30000;
 BbInput::Event(pad_event,false);assert(device==InputDevice::KeyboardMouse);
 std::cout<<"PASS: ordered A/D and W/S rollover, sprint/diagonals, repeat filtering, 65,536 controller axis pairs and mouse gating.\n";
 // Focus loss, text entry and capture cannot leave movement isolation stuck on.
 test_menu=test_navigating=true;clear();key("W");sample();assert(host_keyboard_moving());
 SDL_Event e{};e.type=SDL_EVENT_WINDOW_FOCUS_LOST;BbInput::Event(e,false);assert(!host_keyboard_moving());
 p=sample();assert(p.ly==127&&!p.buttons);
 e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;BbInput::Event(e,false);text_open=true;p=sample();assert(!host_keyboard_moving());
 text_open=false;host_bind_capture_begin(kBindAttack);p=sample();assert(!host_keyboard_moving());
 host_bind_capture_key(SDL_SCANCODE_ESCAPE);host_bind_capture_note_held(false);
 settings.pc_controls=false;p=sample();assert(!host_keyboard_moving());
 std::cout<<"PASS: actual input bridge: DS3 profile, modifier attacks, separate items/interact, diagonal sprint, inventory WASD, Space, title, arrows, controller, mouse clicks, Escape and focus/capture gates.\n";
}
