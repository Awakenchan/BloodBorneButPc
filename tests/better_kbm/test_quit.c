/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production save-aware quit state machine; no guest or GPU. */
#include "../../better-engine/src/runtime_services.c"
#include <assert.h>
static int mounted, quits;
int runtime_savedata_mounted(void) { return mounted; }
bool SDL_PushEvent(SDL_Event *event) { assert(event->type==SDL_EVENT_QUIT);++quits;return true; }
int main(void) {
    assert(!launch_browser("bb:d"));
    runtime_quit_on_open("/app0/dvdroot_ps4/menu/logo_sce.gfx");assert(!quits);
    assert(!launch_browser("bb:p"));assert(launch_browser("bb:d")==1);
    runtime_quit_on_open("/app0/dvdroot_ps4/menu/optionsetting.gfx");assert(!quits);
    mounted=1;runtime_quit_on_open("/app0/dvdroot_ps4/menu/logo_sce.gfx");assert(!quits&&quit_waiting);
    runtime_quit_on_save_done();assert(!quits);
    mounted=0;runtime_quit_on_save_done();assert(quits==1&&!quit_waiting);
    launch_browser("bb:p");assert(launch_browser("bb:d")==1);
    /* A subsequent ordinary Exit Game clears a cancelled desktop request. */
    assert(!launch_browser("bb:d"));launch_browser("bb:t");assert(quits==1);
    launch_browser("bb:p");launch_browser("bb:d");mounted=1;
    launch_browser("bb:t");assert(quits==1&&quit_waiting);
    mounted=0;runtime_quit_on_save_done();assert(quits==2);
    launch_browser("bb:p");launch_browser("bb:d");quit_armed=time(NULL)-121;
    launch_browser("bb:t");assert(quits==2&&!quit_waiting);
    launch_browser("bb:q");assert(quits==3);
    puts("PASS: native Quit to Desktop waits for save unmount, title fallback, cancellation and expiry.");
}
