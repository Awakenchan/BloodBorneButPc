/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime_voice_banks.h"
#include <assert.h>
int main(void) {
    char path[256];
    const char *languages[]={"0","1","2","3","4","5","17","20"};
    const char *suffixes[]={"jaj","eng","frf","ese","ded","iti","ptb","esa"};
    for(size_t i=0;i<8;++i)for(size_t j=0;j<8;++j) {
        char source[256],expected[256];
        snprintf(source,sizeof source,"/app0/dvdroot_ps4/sound/sprj_sm24_%s.fsb",suffixes[j]);
        snprintf(expected,sizeof expected,"/app0/dvdroot_ps4/sound/sprj_sm24_%s.fsb",suffixes[i]);
        int changed=runtime_voice_bank_path(source,languages[i],path,sizeof path);
        assert(changed==(i!=j));if(changed)assert(!strcmp(path,expected));
    }
    assert(runtime_voice_bank_path("/hostapp/dvdroot_ps4/sound/sprj_main_ese.fsb","20",path,sizeof path)==1);
    assert(!strcmp(path,"/hostapp/dvdroot_ps4/sound/sprj_main_esa.fsb"));
    for(const char **p=(const char*[]){"/app0/dvdroot_ps4/sound/sprj_main.fev","/app0/dvdroot_ps4/sound/sprj_m24.fsb","/app0/dvdroot_ps4/sound/sprj_main_xyz.fsb","/data/sprj_main_ese.fsb","/app0/dvdroot_ps4/msg/spaes/menu.msgbnd.dcx",NULL};*p;++p)
        assert(!runtime_voice_bank_path(*p,"20",path,sizeof path));
    assert(!runtime_voice_bank_path("/app0/dvdroot_ps4/sound/sprj_main_ese.fsb","invalid",path,sizeof path));
    assert(runtime_voice_bank_path("/app0/dvdroot_ps4/sound/sprj_main_ese.fsb","20",path,8)==-1);
    puts("PASS: all 64 localized voice-bank selections, main/NPC banks, hostapp, unchanged effects/text/save paths and buffer bounds.");
}
