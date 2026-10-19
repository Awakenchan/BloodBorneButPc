#pragma once
#include <cstring>
#include <string>
#include <vector>
using SDL_Scancode=int;
enum {SDL_SCANCODE_ESCAPE=1,SDL_SCANCODE_DELETE,SDL_SCANCODE_RETURN,SDL_SCANCODE_BACKSPACE,SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,SDL_SCANCODE_INSERT};
inline std::vector<std::string> names={"", "Escape", "Delete", "Return", "Backspace", "Up", "Down", "Left", "Right", "Insert"};
inline int SDL_GetScancodeFromName(const char* n){for(int i=1;i<int(names.size());i++)if(names[i]==n)return i;names.emplace_back(n);return int(names.size())-1;}
inline const char* SDL_GetScancodeName(SDL_Scancode n){return n>0&&n<int(names.size())?names[n].c_str():"";}
enum {SDL_EVENT_WINDOW_FOCUS_LOST=100,SDL_EVENT_WINDOW_FOCUS_GAINED,SDL_EVENT_WINDOW_MOUSE_LEAVE,SDL_EVENT_WINDOW_MOUSE_ENTER,SDL_EVENT_GAMEPAD_BUTTON_DOWN,SDL_EVENT_GAMEPAD_AXIS_MOTION,SDL_EVENT_KEY_DOWN,SDL_EVENT_KEY_UP,SDL_EVENT_MOUSE_MOTION,SDL_EVENT_MOUSE_BUTTON_DOWN,SDL_EVENT_MOUSE_BUTTON_UP,SDL_EVENT_MOUSE_WHEEL};
constexpr unsigned SDL_BUTTON_RMASK=4;
struct SDL_Window{};
union SDL_Event{unsigned type;struct {unsigned type;int scancode;} key;struct{unsigned type;int value;int axis;}gaxis;struct{unsigned type;int button;}gbutton;struct{unsigned type;float x,y,xrel,yrel;}motion;struct{unsigned type;int button;float x,y;}button;struct{unsigned type;float y;}wheel;};
inline void SDL_GetWindowSize(SDL_Window*,int* w,int* h){*w=1920;*h=1080;}
inline bool SDL_SetWindowRelativeMouseMode(SDL_Window*,bool){return true;}
inline void SDL_HideCursor(){}inline void SDL_ShowCursor(){}

enum{SDL_GAMEPAD_BUTTON_COUNT=26,SDL_GAMEPAD_BUTTON_TOUCHPAD=20,SDL_GAMEPAD_AXIS_LEFT_TRIGGER=4,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER=5};
using SDL_GamepadButton=int;
inline const char* SDL_GetGamepadStringForButton(int){return "a";}
