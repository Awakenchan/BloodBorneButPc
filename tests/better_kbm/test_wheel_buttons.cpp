// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheel_buttons.h"
#include <cassert>
#include <cmath>
#include <cstdio>
int main() {
    for(int fps:{30,60,90,120,144,240})for(int phase=0;phase<8;++phase) {
        WheelButtons::Queue q;q.Push(0x20,2);
        bool previous=false;int presses=0;double frame=phase*1000.0/fps/8.0;
        for(int t=0;t<500;++t) {
            const auto bits=q.Poll(t);
            if(t+0.001>=frame){bool down=(bits&0x20)!=0;presses+=down&&!previous;previous=down;frame+=1000.0/fps;}
        }
        assert(presses==2);
    }
    WheelButtons::Queue q;q.Push(0x20,1);q.Push(0x80,1);
    assert(q.Poll(0)==0x20&&q.Poll(79)==0x20);
    assert(q.Poll(80)==0&&q.Poll(129)==0&&q.Poll(130)==0x80);
    q.Reset();assert(q.Poll(131)==0&&q.pending.empty());
    q.Push(0x20,1000);assert(q.pending.size()==32);
    std::puts("PASS: wheel weapon pulses survive 1 kHz pad polling at 30..240 FPS, retain releases/order, bound queues and cancel immediately.");
}
