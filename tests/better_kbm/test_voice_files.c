/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../better-engine/src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void){return &guest_errno;}
int32_t runtime_guest_errno(int e){return e;}
void runtime_memory_note_write(uintptr_t p,uint64_t n){(void)p;(void)n;}
int runtime_memory_guest_writable(uintptr_t p,uint64_t n){(void)p;(void)n;return 1;}
void runtime_quit_on_open(const char *p){(void)p;}
uintptr_t runtime_lookup(const RuntimeExport *t,size_t n,const char *name){
    for(size_t i=0;i<n;++i)if(!strcmp(t[i].name,name))return (uintptr_t)t[i].function;
    return 0;
}
static void make(const char *root,const char *name,const char *content){
    char path[1024];snprintf(path,sizeof path,"%s/%s",root,name);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(content,1,strlen(content),f)==strlen(content));assert(!fclose(f));
}
static void check(const char *name,const char *expected){
    char path[1024],value[64]={0};snprintf(path,sizeof path,"/app0/dvdroot_ps4/sound/%s",name);
    int fd=do_open(path,0,0);assert(fd>=3);assert(do_read(fd,value,sizeof value-1)==(int64_t)strlen(expected));
    assert(!strcmp(value,expected));assert(!do_close(fd));
    GuestStat info;assert(!do_stat(path,&info)&&info.size==(int64_t)strlen(expected));
}
int main(int argc,char **argv){
    assert(argc==2);const char *root=argv[1];
    assert(!runtime_file_mount("/app0/dvdroot_ps4/sound",root));
    make(root,"sprj_main_esa.fsb","LATIN");make(root,"sprj_main_ese.fsb","SPAIN AUDIO");
    make(root,"sprj_sm24_esa.fsb","LATIN NPC");make(root,"sprj_sm24_ese.fsb","SPAIN NPC AUDIO");
    make(root,"sprj_m24.fsb","WORLD EFFECTS");
    assert(!setenv("BB_VOICE_LANGUAGE","20",1));
    check("sprj_main_esa.fsb","LATIN");
    /* Reproduce the supplied log: Spain is requested after the first Latin load. */
    check("sprj_main_ese.fsb","LATIN");check("sprj_sm24_ese.fsb","LATIN NPC");
    check("sprj_m24.fsb","WORLD EFFECTS");
    assert(!setenv("BB_VOICE_LANGUAGE","3",1));check("sprj_main_esa.fsb","SPAIN AUDIO");
    unsetenv("BB_VOICE_LANGUAGE");check("sprj_main_ese.fsb","SPAIN AUDIO");
    assert(!setenv("BB_VOICE_LANGUAGE","20",1));
    make(root,"voice-mod.fsb","LATIN MOD");char mod[1024];snprintf(mod,sizeof mod,"%s/voice-mod.fsb",root);
    assert(!runtime_file_mount("/app0/dvdroot_ps4/sound/sprj_main_esa.fsb",mod));
    check("sprj_main_ese.fsb","LATIN MOD");
    make(root,"sprj_sm25_ese.fsb","WRONG VOICE");
    assert(do_open("/app0/dvdroot_ps4/sound/sprj_sm25_ese.fsb",0,0)==-ENOENT);
    assert(do_open("/app0/dvdroot_ps4/sound/../sprj_sm24_ese.fsb",0,0)==-EACCES);
    puts("PASS: actual guest open/read/stat honors Latin selection after Spain reload, respects selected-language mod mounts, preserves effects and never silently falls back to Spain when Latin is missing.");
}
