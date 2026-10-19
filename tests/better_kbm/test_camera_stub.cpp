// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production assembly store hook without game or renderer services.
#include "mouse_camera_step.h"
#include "mouse_camera_stubs.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
static int calls=0;
extern "C" __attribute__((sysv_abi)) void camera_test_hook(unsigned char* cam,unsigned char* input,float pitch,float yaw){
 ++calls;mouse_camera::turn_camera(cam,input,pitch,yaw);
}
extern "C" __attribute__((sysv_abi)) void drive_camera(unsigned char*,float,float,float,float);
asm(R"(
.text
.globl drive_camera
drive_camera:
 push %rbp
 mov %rsp,%rbp
 push %r13
 sub $1024,%rsp
 mov %rdi,%r13
 vunpcklps %xmm1,%xmm0,%xmm1
 vunpcklps %xmm3,%xmm2,%xmm10
 lea .Lcamera_test_resume(%rip),%rax
 mov %rax,g_mouse_camera_resume(%rip)
 jmp bb_mouse_camera_stub
.Lcamera_test_resume:
 add $1024,%rsp
 pop %r13
 pop %rbp
 ret
)");
static float read(const unsigned char* c,int at){float f;std::memcpy(&f,c+at,4);return f;}
static void write(unsigned char* c,int at,float f){std::memcpy(c+at,&f,4);}
int main(){
 assert(std::abs(mouse_camera::degrees_per_count(5)-0.10f)<1e-6f);
 assert(std::abs(mouse_camera::degrees_per_count(0)-0.04f)<1e-6f);
 assert(std::abs(mouse_camera::degrees_per_count(10)-0.16f)<1e-6f);

 std::array<unsigned char,1024> cam{};g_mouse_camera_host=(void*)&camera_test_hook;
 bb_mouse_camera_enabled=0;
 mouse_camera::g_flags=mouse_camera::kHoldAutoRotation;
 mouse_camera::g_counts=mouse_camera::pack(50,60);mouse_camera::begin_update();
 assert(mouse_camera::g_frame.dx==0&&mouse_camera::g_frame.dy==0&&!mouse_camera::held());
 for(int x=0;x<256;x++)for(int y=0;y<256;y++){
  const float pitch=(x-128)/128.f,yaw=(y-128)/128.f;
  drive_camera(cam.data(),pitch,yaw,pitch,yaw);
  assert(read(cam.data(),0x140)==pitch&&read(cam.data(),0x144)==yaw);
 }
 assert(calls==0);
 bb_mouse_camera_enabled=1;
 const float sensitivity=.25;unsigned bits;std::memcpy(&bits,&sensitivity,4);
 mouse_camera::g_degrees_per_count=bits;mouse_camera::g_flags=0;
 mouse_camera::g_counts=mouse_camera::pack(10,20);mouse_camera::begin_update();
 write(cam.data(),0x1ec,1);write(cam.data(),0x1f0,-1);
 drive_camera(cam.data(),0,0,0,0);
 assert(calls==1&&read(cam.data(),0x140)>0&&read(cam.data(),0x144)>0);
 std::puts("PASS: real camera assembly bypasses host callback for 65,536 controller angle pairs; mouse step still changes both axes when enabled.");
}
