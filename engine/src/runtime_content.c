#include "runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define NOT_INITIALIZED ((int32_t)0x80d90001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static int32_t require_initialized(void) {
    require_provider();
    if (!refs || !initialized) { fputs("Runtime: AppContent query before initialization\n",stderr); return NOT_INITIALIZED; }
    return 0;
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    int32_t result=require_initialized();
    if (result) return result;
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
#define LABEL_SIZE 17
#define MAX_ADDONS 64
#define ADDON_INSTALLED 4u
typedef struct { char label[LABEL_SIZE]; char padding[3]; uint32_t status; } AddonInfo;
static char addon_labels[MAX_ADDONS][LABEL_SIZE];
static uint32_t addon_count;
static int addons_found;
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static int sfo_string(const char *path,const char *key,char *out,size_t size) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char *sfo=malloc(65536);
    size_t n=sfo ? fread(sfo,1,65536,f) : 0;
    fclose(f);
    int found=0;
    if (n>=20 && !memcmp(sfo,"\0PSF",4)) {
        const uint32_t keys=le32(sfo+8), data=le32(sfo+12), count=le32(sfo+16);
        for (uint32_t i=0;i<count && 20+16*((size_t)i+1)<=n && !found;++i) {
            const unsigned char *e=sfo+20+16*(size_t)i;
            const size_t k=(size_t)keys+(e[0]|e[1]<<8), v=(size_t)data+le32(e+12), len=le32(e+4);
            if (k>=n || !memchr(sfo+k,0,n-k) || strcmp((const char *)sfo+k,key)) continue;
            if (v>=n || len>n-v || !size) break;
            const size_t m=len<size ? len : size-1;
            memcpy(out,sfo+v,m); out[m]=0; found=1;
        }
    }
    free(sfo);
    return found;
}
static void add_addon(const char *path,const char *name,int need_sfo) {
    char sfo[4096], category[16]="", id[64]="", label[64]="";
    if (snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",path)>=(int)sizeof(sfo)) return;
    if (sfo_string(sfo,"CATEGORY",category,sizeof(category))) {
        if (strcmp(category,"ac")) return;
        if (sfo_string(sfo,"CONTENT_ID",id,sizeof(id))) {
            const char *dash=strrchr(id,'-');
            snprintf(label,sizeof(label),"%s",dash ? dash+1 : "");
        }
    } else if (need_sfo) return;
    if (!label[0]) snprintf(label,sizeof(label),"%s",name);
    const size_t n=strlen(label);
    if (!n || n>=LABEL_SIZE || addon_count>=MAX_ADDONS) return;
    for (size_t i=0;i<n;++i) if (!isalnum((unsigned char)label[i])) return;
    for (uint32_t i=0;i<addon_count;++i) if (!strcmp(addon_labels[i],label)) return;
    memcpy(addon_labels[addon_count++],label,n+1);
    printf("Runtime: add-on %s reported installed (%s)\n",label,path);
}
static void scan_addons(const char *folder) {
    DIR *dir=opendir(folder);
    for (struct dirent *e; dir && (e=readdir(dir));) {
        char path[4096];
        struct stat st;
        if (e->d_name[0]=='.' || snprintf(path,sizeof(path),"%s/%s",folder,e->d_name)>=(int)sizeof(path) ||
            stat(path,&st) || !S_ISDIR(st.st_mode)) continue;
        add_addon(path,e->d_name,0);
    }
    if (dir) closedir(dir);
}
static void find_addons(void) {
    if (addons_found) return;
    addons_found=1;
    const char *enabled=getenv("BB_DLC");
    if (enabled && enabled[0]=='0') { puts("Runtime: DLC switched off: no add-ons reported"); return; }
    const char *dlc=getenv("BB_DLC_DIR");
    if (dlc && *dlc) {
        add_addon(dlc,"",1);
        scan_addons(dlc);
    }
    char root[4096];
    DIR *titles=snprintf(root,sizeof(root),"%s/addcont",runtime_file_user_dir())<(int)sizeof(root) ? opendir(root) : NULL;
    for (struct dirent *t; titles && (t=readdir(titles));) {
        char title[4096];
        if (t->d_name[0]=='.' || snprintf(title,sizeof(title),"%s/%s",root,t->d_name)>=(int)sizeof(title)) continue;
        scan_addons(title);
    }
    if (titles) closedir(titles);
    if (dlc && *dlc && !addon_count) printf("Runtime: DLC folder %s: no add-ons found\n",dlc);
    if (enabled && enabled[0]=='1' && !addon_count) add_addon("","SPEXPANSIONDLC03",0);
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    int32_t result=require_initialized();
    if (result) return result;
    static unsigned noted;
    if (service && !noted++) fprintf(stderr,"Runtime: AppContent service label %u: no add-ons\n",service);
    if ((!capacity || !list) && !hits) return PARAMETER;
    find_addons();
    uint32_t count=service ? 0 : addon_count;
    if (list && capacity) {
        if (count>capacity) count=capacity;
        for (uint32_t i=0;i<count;++i) {
            AddonInfo info;
            memset(&info,0,sizeof(info));
            memcpy(info.label,addon_labels[i],strlen(addon_labels[i]));
            info.status=ADDON_INSTALLED;
            memcpy((unsigned char *)list+(size_t)i*sizeof(info),&info,sizeof(info));
        }
    }
    if (hits) *hits=count;
    ++lists; return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#M#N")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#M#N")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#M#N")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#c#d")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#c#d")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#c#d")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
