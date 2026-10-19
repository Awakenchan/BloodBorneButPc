// SPDX-License-Identifier: GPL-3.0-or-later
#include "sprint_turn.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    constexpr std::array<SprintTurn::Axes,8> directions = {{{0,-1},{1,-1},{1,0},{1,1},
                                                          {0,1},{-1,1},{-1,0},{-1,-1}}};
    int turns=0,reversals=0;
    for (int fps : {30,60,90,120,144,240}) {
        for (auto from : directions) for (auto to : directions) {
            SprintTurn::State state;
            auto at = std::int64_t(1000000);
            auto initial = SprintTurn::Apply(state,from.x,from.y,true,at);
            assert(initial.x==from.x*127 && initial.y==from.y*127);
            const bool opposite = from.x*to.x<0 || from.y*to.y<0;
            for (int tick=1;tick<=fps;++tick) {
                const auto before=state.angle;
                at += 1000000/fps;
                auto next=SprintTurn::Apply(state,to.x,to.y,true,at);
                // Never pass through neutral or reduce the fully pushed axis.
                assert(std::max(std::abs(next.x),std::abs(next.y))==127);
                assert(next.x>=-127&&next.x<=127&&next.y>=-127&&next.y<=127);
                if (opposite && tick==1) {
                    assert(next.x==to.x*127 && next.y==to.y*127);
                    ++reversals;
                } else if (!opposite) {
                    constexpr double pi=3.14159265358979323846;
                    const double delta=std::abs(std::remainder(state.angle-before,2*pi));
                    assert(delta<=pi/6+1e-9);
                }
                if (tick==fps) assert(next.x==to.x*127 && next.y==to.y*127);
            }
            if (!opposite) ++turns;
            auto stopped=SprintTurn::Apply(state,0,0,true,at+1000);
            assert(stopped.x==0&&stopped.y==0&&!state.active);
        }
    }
    // A reversal in the middle of a diagonal transition must still snap.
    SprintTurn::State state;
    SprintTurn::Apply(state,0,-1,true,1000000);
    auto halfway=SprintTurn::Apply(state,-1,-1,true,1010000);
    assert(halfway.x<0 && halfway.x>-127 && halfway.y==-127);
    auto reverse=SprintTurn::Apply(state,1,-1,true,1011000);
    assert(reverse.x==127&&reverse.y==-127);
    auto immediate=SprintTurn::Apply(state,-1,0,false,1012000);
    assert(immediate.x==-127&&immediate.y==0&&!state.active);
    SprintTurn::Apply(state,0,-1,true,1020000);
    auto repeated=SprintTurn::Apply(state,-1,-1,true,1020000);
    assert(repeated.x==0&&repeated.y==-127);
    state={};
    SprintTurn::Config legacy{900,60,false};
    SprintTurn::Apply(state,0,-1,true,2000000,legacy);
    auto nine=SprintTurn::Apply(state,-1,-1,true,2010000,legacy);
    constexpr double pi=3.14159265358979323846;
    assert(std::abs(state.angle-(-pi/2-9*pi/180))<1e-9);
    auto gap=SprintTurn::Apply(state,0,0,true,2040000,legacy);
    assert(gap.x<0&&gap.y==-127);
    auto expired=SprintTurn::Apply(state,0,0,true,2070001,legacy);
    assert(expired.x==0&&expired.y==0&&!state.active);
    SprintTurn::Apply(state,-1,0,true,3000000,legacy);
    SprintTurn::Apply(state,0,0,true,3010000,legacy);
    auto reverse_after_gap=SprintTurn::Apply(state,1,0,true,3020000,legacy);
    assert(reverse_after_gap.x==127&&reverse_after_gap.y==0);
    auto release_sprint=SprintTurn::Apply(state,0,0,false,3020001,legacy);
    assert(release_sprint.x==0&&release_sprint.y==0);
    state={};SprintTurn::Config circle{900,0,true};
    auto circular=SprintTurn::Apply(state,1,1,true,4000000,circle);
    assert(std::abs(std::hypot(double(circular.x),double(circular.y))-127)<1);
    state={};SprintTurn::Apply(state,0,-1,true,5000000,{0,0,false});
    auto instant=SprintTurn::Apply(state,-1,-1,true,5000001,{0,0,false});
    assert(instant.x==-127&&instant.y==-127);
    std::printf("PASS: %d compatible sprint turns and %d immediate opposing reversals at 30/60/90/120/144/240 Hz; full intensity, immediate stop, no controller path.\n",turns,reversals);
}
