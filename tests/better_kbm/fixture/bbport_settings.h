#pragma once
#include <atomic>
namespace BbSettings {inline constexpr int OutputCount=1;struct State {
 bool pc_controls=true,mouse_camera=true,mouse_invert_x=false,mouse_invert_y=false,mouse_auto_rotation=false,widescreen=false;
 int stick_turn_rate=1200,stick_release_delay_ms=60;bool stick_circular=true;
 std::atomic<bool> mouse_menu{true};int mouse_sensitivity=5,startup_output_res=0;std::atomic<int> output_res{0};
};inline State& Get(){static State s;return s;}inline bool ResolutionNeedsRestart(){return false;}
inline int OutputWidth(int){return 1920;}inline int OutputHeight(int){return 1080;}inline void Save(){} }
