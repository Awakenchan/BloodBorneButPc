#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
typedef struct _stat64 HostStat;
#define host_stat _stat64
#define host_fstat _fstat64
#define lseek _lseeki64
#else
typedef struct stat HostStat;
#define host_stat stat
#define host_fstat fstat
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES 1024
#define MAX_MOUNTS 64

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"FreeBSD stat layout");

typedef struct { char *names; size_t count, *offsets; unsigned char *types; } Listing;
typedef struct { int used, host; Listing *dir; size_t position; uint64_t opened_ns; int loader; char path[512]; } File;
typedef struct { char guest[64]; char host[512]; } Mount;
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static size_t mount_count, opens, reads, writes, missing;
static uint64_t bytes_read;
static HostMutex lock=HOST_MUTEX_INIT;

int runtime_file_mount(const char *guest,const char *host) {
    host_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        snprintf(mounts[i].host,sizeof(mounts[i].host),"%s",host);
        host_unlock(&lock); return 0;
    }
    if (mount_count==MAX_MOUNTS || strlen(guest)>=64 || strlen(host)>=512) { host_unlock(&lock); return -1; }
    snprintf(mounts[mount_count].guest,64,"%s",guest);
    snprintf(mounts[mount_count].host,512,"%s",host);
    ++mount_count;
    host_unlock(&lock);
    return 0;
}
void runtime_file_unmount(const char *guest) {
    host_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        mounts[i]=mounts[--mount_count]; break;
    }
    host_unlock(&lock);
}
static char user_root[512]="user";
const char *runtime_file_user_dir(void) { return user_root; }
static void stream_configure(const char *app0);

typedef struct { char *key, *host; } ModFile;
static ModFile *mod_files;
static size_t mod_file_count;
static void fold_path(char *s) {
    for (; *s; ++s) *s=*s=='\\' ? '/' : (*s>='A' && *s<='Z') ? (char)(*s-'A'+'a') : *s;
}
static int mod_file_compare(const void *a,const void *b) {
    return strcmp(((const ModFile *)a)->key,((const ModFile *)b)->key);
}
static void mod_files_load(void) {
    const char *list=getenv("BB_MOD_FILES");
    FILE *f=list && *list ? fopen(list,"rb") : NULL;
    if (!f) return;
    char line[2048];
    size_t capacity=0;
    while (fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;
        char *tab=strchr(line,'\t');
        if (!tab || tab==line || !tab[1]) continue;
        *tab=0;
        if (mod_file_count==capacity) {
            capacity=capacity ? capacity*2 : 256;
            ModFile *grown=realloc(mod_files,capacity*sizeof(*grown));
            if (!grown) break;
            mod_files=grown;
        }
        mod_files[mod_file_count].key=strdup(line);
        mod_files[mod_file_count].host=strdup(tab+1);
        if (!mod_files[mod_file_count].key || !mod_files[mod_file_count].host) break;
        fold_path(mod_files[mod_file_count].key);
        ++mod_file_count;
    }
    fclose(f);
    qsort(mod_files,mod_file_count,sizeof(*mod_files),mod_file_compare);
    printf("Runtime: %zu mod files read in place (%s)\n",mod_file_count,list);
}
static const char *mod_file_for(const char *guest) {
    if (!mod_file_count) return NULL;
    const char *rest=!strncmp(guest,"/app0/",6) ? guest+6 : !strncmp(guest,"/hostapp/",9) ? guest+9 : NULL;
    if (!rest || strlen(rest)>=1024) return NULL;
    char key[1024];
    snprintf(key,sizeof(key),"%s",rest);
    fold_path(key);
    const ModFile probe={key,NULL};
    const ModFile *found=bsearch(&probe,mod_files,mod_file_count,sizeof(*mod_files),mod_file_compare);
    return found ? found->host : NULL;
}
static void file_fixes_load(void) {
    const char *list=getenv("BB_FILE_FIXES");
    FILE *f=list && *list ? fopen(list,"rb") : NULL;
    if (!f) return;
    char line[2048],guest[1100];
    int mounted=0;
    while (fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;
        char *tab=strchr(line,'\t');
        if (!tab || tab==line || !tab[1]) continue;
        *tab=0;
        snprintf(guest,sizeof(guest),"/app0/%s",line);
        if (!runtime_file_mount(guest,tab+1)) ++mounted;
        else printf("Runtime: the fixed %s cannot be mounted\n",line);
    }
    fclose(f);
    if (mounted) printf("Runtime: %d game files fixed (BB_FILE_FIXES)\n",mounted);
}
void runtime_file_configure(const char *app0,const char *user) {
    char path[600];
    stream_configure(app0);
    mod_files_load();
    snprintf(user_root,sizeof(user_root),"%s",user);
    runtime_file_mount("/app0",app0);
    runtime_file_mount("/hostapp",app0);
    file_fixes_load();
    const char *writable[]={"temp0","download0","data"};
    mkdir(user,0755);
    snprintf(path,sizeof(path),"%s/.write-test",user);
    FILE *probe=fopen(path,"wb");
    if (probe) { fclose(probe); remove(path); }
    else printf("Saves: the saves folder %s cannot be written (%s): the game cannot create or keep "
                "saves. Pick another saves folder in the launcher%s\n",user,strerror(errno),
#ifdef _WIN32
                ", or allow Lantern Engine in Windows Security > Ransomware protection > "
                "Controlled folder access"
#else
                ""
#endif
                );
    for (int i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/%s",user,writable[i]);
        mkdir(path,0755);
        char guest[32]; snprintf(guest,sizeof(guest),"/%s",writable[i]);
        runtime_file_mount(guest,path);
    }
}
static int translate(const char *guest,char *out,size_t size) {
    if (!guest || !*guest) return ENOENT;
    char buffer[1024];
    if (guest[0]!='/') snprintf(buffer,sizeof(buffer),"/app0/%s",guest);
    else snprintf(buffer,sizeof(buffer),"%s",guest);

    for (const char *p=buffer;(p=strstr(p,".."));p+=2)
        if ((p==buffer || p[-1]=='/') && (p[2]==0 || p[2]=='/')) return EACCES;
    const char *mod=mod_file_for(buffer);
    if (mod) return (size_t)snprintf(out,size,"%s",mod)>=size ? ENAMETOOLONG : 0;
    host_lock(&lock);
    size_t best=0; const Mount *m=NULL;
    for (size_t i=0;i<mount_count;++i) {
        size_t n=strlen(mounts[i].guest);
        if (!strncmp(buffer,mounts[i].guest,n) && (buffer[n]=='/' || !buffer[n]) && n>best) { best=n; m=&mounts[i]; }
    }
    int result=0;
    if (!m) result=ENOENT;
    else if ((size_t)snprintf(out,size,"%s%s",m->host,buffer+best)>=size) result=ENAMETOOLONG;
    host_unlock(&lock);
    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\n",guest);
    return result;
}
static int host_flags(int flags) {
    int r;
    switch (flags&3) { case 0: r=O_RDONLY; break; case 1: r=O_WRONLY; break; default: r=O_RDWR; }
    if (flags&0x4) r|=O_NONBLOCK;
    if (flags&0x8) r|=O_APPEND;
    if (flags&0x80) r|=O_SYNC;
    if (flags&0x200) r|=O_CREAT;
    if (flags&0x400) r|=O_TRUNC;
    if (flags&0x800) r|=O_EXCL;
    if (flags&0x20000) r|=O_DIRECTORY;
#ifdef _WIN32
    r|=O_BINARY;
#endif
    return r|O_CLOEXEC;
}
static void free_listing(Listing *l) { if (l) { free(l->names); free(l->offsets); free(l->types); free(l); } }
static Listing *list_directory(const char *path) {
    DIR *d=opendir(path);
    if (!d) return NULL;
    Listing *l=calloc(1,sizeof(*l));
    size_t capacity=0,bytes=0,cap_names=0;
    struct dirent *e;
    while (l && (e=readdir(d))) {
        size_t n=strlen(e->d_name)+1;
        if (l->count==capacity) {
            capacity=capacity ? capacity*2 : 64;
            l->offsets=realloc(l->offsets,capacity*sizeof(size_t));
            l->types=realloc(l->types,capacity);
        }
        if (bytes+n>cap_names) { cap_names=(bytes+n)*2; l->names=realloc(l->names,cap_names); }
        if (!l->offsets || !l->types || !l->names) { fputs("Out of memory listing directory\n",stderr); exit(1); }
        memcpy(l->names+bytes,e->d_name,n);
        l->offsets[l->count]=bytes;
#ifdef _WIN32
        char child[1300]; HostStat entry;
        snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        l->types[l->count]=host_stat(child,&entry) ? 0 : S_ISDIR(entry.st_mode) ? 4 : S_ISREG(entry.st_mode) ? 8 : 0;
#else
        unsigned char type=e->d_type;
        if (type==DT_LNK || type==DT_UNKNOWN) {
            struct stat entry;
            if (!fstatat(dirfd(d),e->d_name,&entry,0))
                type=S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : type;
        }
        l->types[l->count]=type==DT_DIR ? 4 : type==DT_REG ? 8 : type==DT_LNK ? 10 : 0;
#endif
        ++l->count; bytes+=n;
    }
    closedir(d);
    return l;
}
static void convert_stat(const HostStat *s,GuestStat *g) {
    memset(g,0,sizeof(*g));
    g->dev=(uint32_t)s->st_dev; g->ino=(uint32_t)s->st_ino;
    g->nlink=(uint16_t)s->st_nlink;
    g->size=s->st_size;
#ifdef _WIN32
    g->mode=(uint16_t)((S_ISDIR(s->st_mode) ? 0040000 : 0100000) | ((s->st_mode & 0700) * 0111 & 0777));
    g->blocks=(s->st_size+511)/512; g->blksize=65536;
    g->atime=(GuestTimespec){s->st_atime,0};
    g->mtime=(GuestTimespec){s->st_mtime,0};
    g->ctime=(GuestTimespec){s->st_ctime,0};
#else
    g->mode=(uint16_t)s->st_mode;
    g->blocks=s->st_blocks; g->blksize=(uint32_t)s->st_blksize;
    g->atime=(GuestTimespec){s->st_atim.tv_sec,s->st_atim.tv_nsec};
    g->mtime=(GuestTimespec){s->st_mtim.tv_sec,s->st_mtim.tv_nsec};
    g->ctime=(GuestTimespec){s->st_ctim.tv_sec,s->st_ctim.tv_nsec};
#endif
    g->birthtime=g->ctime;
}
static File *get(int fd) {
    if (fd<3 || fd>=MAX_FILES || !files[fd].used) return NULL;
    return &files[fd];
}
static int audio_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("BB_AUDIO_TRACE"); v=e && e[0]=='1'; } return v; }
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    if (*p!='/') return 1;
    return (!strncmp(p,"/app0",5) && (!p[5] || p[5]=='/')) ||
           (!strncmp(p,"/hostapp",8) && (!p[8] || p[8]=='/'));
}
static int stream_hdd=-1;
static int stream_sequential, stream_prefetch;
static uint64_t st_reads, st_bytes, st_read_ns, st_read_max_ns, st_fast_reads, st_opens, st_open_ns,
                st_requests, st_request_ns, st_request_max_ns, st_other_bytes, st_inflight,
                pf_files, pf_bytes, pf_dirs, st_window_start;
static void atomic_max_u64(uint64_t *p,uint64_t v) {
    uint64_t c=__atomic_load_n(p,__ATOMIC_RELAXED);
    while (v>c && !__atomic_compare_exchange_n(p,&c,v,1,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
}
static int env_mode(const char *name,int automatic) {
    const char *e=getenv(name);
    if (e && e[0]=='0') return 0;
    if (e && e[0]=='1') return 1;
    return automatic;
}
#ifdef _WIN32
#include <winioctl.h>
static uint64_t env_mb(const char *name,uint64_t fallback) {
    const char *e=getenv(name);
    return (e && *e ? strtoull(e,NULL,10) : fallback)<<20;
}
typedef struct { DWORD property, query; BYTE extra[4]; } SeekPenaltyQuery;
typedef struct { DWORD version, size; BOOLEAN incurs_seek_penalty; } SeekPenaltyAnswer;
static int seek_penalty(const char *path) {
    char volume[MAX_PATH];
    if (!GetVolumePathNameA(path,volume,sizeof(volume))) return -1;
    size_t n=strlen(volume);
    if (n && (volume[n-1]=='\\' || volume[n-1]=='/')) volume[--n]=0;
    if (n!=2 || volume[1]!=':') return -1;
    const char device[]={'\\','\\','.','\\',volume[0],':',0};
    HANDLE h=CreateFileA(device,0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if (h==INVALID_HANDLE_VALUE) return -1;
    SeekPenaltyQuery query={7,0,{0}};
    SeekPenaltyAnswer answer={0,0,0};
    DWORD got=0;
    BOOL ok=DeviceIoControl(h,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),&answer,sizeof(answer),&got,NULL);
    CloseHandle(h);
    return ok && got>offsetof(SeekPenaltyAnswer,incurs_seek_penalty) ? answer.incurs_seek_penalty!=0 : -1;
}

enum { PF_QUEUE=32, PF_SEEN=1024, PF_CHUNK=1<<20 };
static HostMutex pf_lock=HOST_MUTEX_INIT;
static HostCond pf_cond=HOST_COND_INIT;
static char pf_queue[PF_QUEUE][520];
static unsigned pf_head, pf_tail, pf_seen_count;
static uint64_t pf_seen[PF_SEEN], pf_dir_cap, pf_file_cap;
static int pf_started;
static void prefetch_dir(const char *dir,unsigned char *buffer) {
    char pattern[600];
    snprintf(pattern,sizeof(pattern),"%s\\*",dir);
    WIN32_FIND_DATAA e;
    HANDLE find=FindFirstFileA(pattern,&e);
    if (find==INVALID_HANDLE_VALUE) return;
    uint64_t total=0;
    const DWORD skip=FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_OFFLINE|0x00400000  ;
    do {
        if (e.dwFileAttributes & skip) continue;
        const uint64_t size=((uint64_t)e.nFileSizeHigh<<32)|e.nFileSizeLow;
        if (!size || size>pf_file_cap) continue;
        if (total+size>pf_dir_cap) break;
        char path[900];
        snprintf(path,sizeof(path),"%s\\%s",dir,e.cFileName);
        HANDLE f=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,
                             OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (f==INVALID_HANDLE_VALUE) continue;
        for (;;) {
            for (int i=0;i<4 && __atomic_load_n(&st_inflight,__ATOMIC_RELAXED);++i) Sleep(1);
            DWORD n=0;
            if (!ReadFile(f,buffer,PF_CHUNK,&n,NULL) || !n) break;
            __atomic_add_fetch(&pf_bytes,n,__ATOMIC_RELAXED);
            if (n<PF_CHUNK) break;
        }
        CloseHandle(f);
        __atomic_add_fetch(&pf_files,1,__ATOMIC_RELAXED);
        total+=size;
    } while (FindNextFileA(find,&e));
    FindClose(find);
    __atomic_add_fetch(&pf_dirs,1,__ATOMIC_RELAXED);
}
static void *prefetch_main(void *unused) {
    (void)unused;
    runtime_win_set_thread_name("bb:Prefetch");
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    typedef BOOL (WINAPI *SetInfo)(HANDLE,int,LPVOID,DWORD);
    SetInfo set_info=(SetInfo)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"SetThreadInformation");
    if (set_info) { ULONG priority=2; set_info(GetCurrentThread(),0,&priority,sizeof(priority)); }
    unsigned char *buffer=malloc(PF_CHUNK);
    if (!buffer) return NULL;
    char dir[520];
    for (;;) {
        host_lock(&pf_lock);
        while (pf_head==pf_tail) host_cond_wait(&pf_cond,&pf_lock);
        memcpy(dir,pf_queue[pf_head%PF_QUEUE],sizeof(dir));
        ++pf_head;
        host_unlock(&pf_lock);
        prefetch_dir(dir,buffer);
    }
    return NULL;
}
static void prefetch_note(const char *guest,const char *host) {
    const char *m=strstr(guest,"/map/m");
    if (!m || !strchr(m+5,'/')) return;
    const char *slash=strrchr(host,'/'), *back=strrchr(host,'\\');
    if (back>slash) slash=back;
    if (!slash || (size_t)(slash-host)>=sizeof(pf_queue[0])) return;
    uint64_t h=0xcbf29ce484222325ull;
    for (const char *p=host;p<slash;++p) h=(h^(unsigned char)(*p=='/' ? '\\' : *p))*0x100000001b3ull;
    host_lock(&pf_lock);
    for (unsigned i=0;i<pf_seen_count;++i) if (pf_seen[i]==h) { host_unlock(&pf_lock); return; }
    if (pf_seen_count==PF_SEEN || pf_tail-pf_head==PF_QUEUE) { host_unlock(&pf_lock); return; }
    pf_seen[pf_seen_count++]=h;
    char *slot=pf_queue[pf_tail%PF_QUEUE];
    memcpy(slot,host,(size_t)(slash-host)); slot[slash-host]=0;
    for (char *p=slot;*p;++p) if (*p=='/') *p='\\';
    ++pf_tail;
    int start=!pf_started; pf_started=1;
    host_cond_signal(&pf_cond);
    host_unlock(&pf_lock);
    if (start) {
        HostThread t;
        if (!host_thread_start(&t,256*1024,prefetch_main,NULL)) host_thread_detach(t);
    }
}
#else
static int seek_penalty(const char *path) { (void)path; return -1; }
static void prefetch_note(const char *guest,const char *host) { (void)guest; (void)host; }
#endif
static void stream_configure(const char *app0) {
    stream_hdd=seek_penalty(app0);
    stream_sequential=env_mode("BB_FILE_SEQUENTIAL",stream_hdd==1);
    stream_prefetch=env_mode("BB_FILE_PREFETCH",stream_hdd==1);
#ifdef _WIN32
    pf_dir_cap=env_mb("BB_FILE_PREFETCH_MB",1024);
    pf_file_cap=env_mb("BB_FILE_PREFETCH_FILE_MB",32);
#else
    stream_sequential=stream_prefetch=0;
#endif
    st_window_start=host_monotonic_ns();
    printf("Streaming: game files on %s; read-ahead hint %s (BB_FILE_SEQUENTIAL), map area prefetch %s (BB_FILE_PREFETCH)\n",
           stream_hdd==1 ? "a hard disk (seek penalty): models and textures load late while walking; an SSD loads them several times faster"
           : stream_hdd==0 ? "a solid-state disk" : "a disk of unknown type",
           stream_sequential ? "on" : "off", stream_prefetch ? "on" : "off");
}
void runtime_file_stream_report(void) {
    const uint64_t now=host_monotonic_ns();
    const uint64_t start=__atomic_exchange_n(&st_window_start,now,__ATOMIC_RELAXED);
    const double window=start && now>start ? (now-start)/1e9 : 5.0;
    const uint64_t reads=__atomic_exchange_n(&st_reads,0,__ATOMIC_RELAXED),
                   bytes=__atomic_exchange_n(&st_bytes,0,__ATOMIC_RELAXED),
                   read_ns=__atomic_exchange_n(&st_read_ns,0,__ATOMIC_RELAXED),
                   read_max=__atomic_exchange_n(&st_read_max_ns,0,__ATOMIC_RELAXED),
                   fast=__atomic_exchange_n(&st_fast_reads,0,__ATOMIC_RELAXED),
                   opens=__atomic_exchange_n(&st_opens,0,__ATOMIC_RELAXED),
                   open_ns=__atomic_exchange_n(&st_open_ns,0,__ATOMIC_RELAXED),
                   requests=__atomic_exchange_n(&st_requests,0,__ATOMIC_RELAXED),
                   request_ns=__atomic_exchange_n(&st_request_ns,0,__ATOMIC_RELAXED),
                   request_max=__atomic_exchange_n(&st_request_max_ns,0,__ATOMIC_RELAXED),
                   other=__atomic_exchange_n(&st_other_bytes,0,__ATOMIC_RELAXED),
                   prefetched=__atomic_exchange_n(&pf_bytes,0,__ATOMIC_RELAXED),
                   prefetched_files=__atomic_exchange_n(&pf_files,0,__ATOMIC_RELAXED);
    if (!reads && !opens && !prefetched) return;
    printf("Streaming: loaders read %.1f MB/s (%llu reads, %.0f KiB avg, %.2f ms avg, longest %.1f ms, %.0f%% under 1 ms; "
           "reading %.0f%% of the time); %llu files opened (%.2f ms to open), %llu closed %.1f ms after opening (longest %.0f ms); "
           "other threads %.1f MB/s; prefetch %.1f MB/s (%llu files, %llu folders so far)%s\n",
           bytes/(window*1e6),(unsigned long long)reads,reads ? bytes/1024.0/reads : 0.0,reads ? read_ns/1e6/reads : 0.0,
           read_max/1e6,reads ? 100.0*fast/reads : 0.0,100.0*read_ns/(window*1e9),
           (unsigned long long)opens,opens ? open_ns/1e6/opens : 0.0,
           (unsigned long long)requests,requests ? request_ns/1e6/requests : 0.0,request_max/1e6,
           other/(window*1e6),prefetched/(window*1e6),(unsigned long long)prefetched_files,
           (unsigned long long)__atomic_load_n(&pf_dirs,__ATOMIC_RELAXED),
           stream_hdd==1 ? "; game on a hard disk" : "");
}
static uint64_t stream_read_begin(int loader) {
    if (!loader) return 0;
    __atomic_add_fetch(&st_inflight,1,__ATOMIC_RELAXED);
    return host_monotonic_ns();
}
static void stream_read_end(int loader,uint64_t start,int64_t n) {
    if (!loader) { if (n>0) __atomic_add_fetch(&st_other_bytes,(uint64_t)n,__ATOMIC_RELAXED); return; }
    const uint64_t ns=host_monotonic_ns()-start;
    __atomic_sub_fetch(&st_inflight,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&st_reads,1,__ATOMIC_RELAXED);
    if (n>0) __atomic_add_fetch(&st_bytes,(uint64_t)n,__ATOMIC_RELAXED);
    __atomic_add_fetch(&st_read_ns,ns,__ATOMIC_RELAXED);
    if (ns<1000000) __atomic_add_fetch(&st_fast_reads,1,__ATOMIC_RELAXED);
    atomic_max_u64(&st_read_max_ns,ns);
}
static int fd_is_loader(int fd) { File *f=get(fd); return f && f->loader; }

static int64_t do_open(const char *guest,int flags,int mode) {
    if (game_path(guest) && (flags & (3|0x8|0x200|0x400|0x800))) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    HostStat s;
    Listing *dir=NULL;
    int host=-1;
    const int game_read=game_path(guest) && !(flags&3);
    const int loader=game_read && runtime_thread_is_loader();
    const uint64_t open_start=host_monotonic_ns();
#ifdef _WIN32
    if (!host_stat(path,&s) && S_ISDIR(s.st_mode)) {
        if (flags&3) return -EISDIR;
        if (!(dir=list_directory(path))) return -errno;
    } else
#endif
    {
    int hflags=host_flags(flags);
#ifdef _O_SEQUENTIAL
    if (game_read && stream_sequential) hflags|=_O_SEQUENTIAL;
#endif
    host=open(path,hflags,mode ? mode : 0644);
    if (host<0) {
        e=errno;
        if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
        return -e;
    }
    if (!host_fstat(host,&s) && S_ISDIR(s.st_mode)) dir=list_directory(path);
    }
    if (loader && !dir) {
        __atomic_add_fetch(&st_opens,1,__ATOMIC_RELAXED);
        __atomic_add_fetch(&st_open_ns,host_monotonic_ns()-open_start,__ATOMIC_RELAXED);
        if (stream_prefetch) prefetch_note(guest,path);
    }
    host_lock(&lock);
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd<0) { host_unlock(&lock); if (host>=0) close(host); free_listing(dir); return -EMFILE; }
    files[fd]=(File){.used=1,.host=host,.dir=dir,.opened_ns=open_start,.loader=loader && !dir};
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
    ++opens;
    host_unlock(&lock);
    if (audio_trace() && strstr(guest,"sound/")) printf("Audio trace: open(%s) -> fd %d, %lld bytes\n",guest,fd,(long long)s.st_size);
    const char *mod_trace=getenv("BB_MOD_TRACE"), *mod_root=getenv("BB_MODS_DIR");
    if (mod_trace && mod_trace[0]=='1' && mod_root) {
        char actual[PATH_MAX],root[PATH_MAX];
        static unsigned traced;
        if (realpath(path,actual) && realpath(mod_root,root)) {
            size_t n=strlen(root);
            if (!strncmp(actual,root,n) && (actual[n]=='/' || actual[n]=='\\') &&
                __atomic_fetch_add(&traced,1,__ATOMIC_RELAXED)<32)
                printf("Mods: open %s -> %s\n",guest,actual);
        }
    }
    return fd;
}
static int64_t do_close(int fd) {
    if (fd>=0 && fd<3) return 0;
    host_lock(&lock);
    File *f=get(fd);
    if (!f) { host_unlock(&lock); return -EBADF; }
    if (f->loader) {
        const uint64_t ns=host_monotonic_ns()-f->opened_ns;
        __atomic_add_fetch(&st_requests,1,__ATOMIC_RELAXED);
        __atomic_add_fetch(&st_request_ns,ns,__ATOMIC_RELAXED);
        atomic_max_u64(&st_request_max_ns,ns);
    }
    if (f->host>=0) close(f->host);
    free_listing(f->dir);
    *f=(File){0};
    host_unlock(&lock);
    return 0;
}
static int host_fd(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    return f ? f->host : -1;
}
static void touch_for_write(void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096) {
        volatile unsigned char *b=(volatile unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
        *b=*b;
    }
}
static ssize_t read_bounced(int h,void *buffer,uint64_t size,int64_t offset) {
    enum { BOUNCE=1<<20 };
    unsigned char *bounce=malloc(size<BOUNCE ? size : BOUNCE);
    if (!bounce) { errno=ENOMEM; return -1; }
    uint64_t done=0;
    while (done<size) {
        const uint64_t chunk=size-done<BOUNCE ? size-done : BOUNCE;
        const ssize_t n=offset<0 ? read(h,bounce,chunk) : pread(h,bounce,chunk,offset+(int64_t)done);
        if (n<0) {
            if (done) break;
            int e=errno; free(bounce); errno=e;
            return -1;
        }
        memcpy((unsigned char *)buffer+done,bounce,(size_t)n);
        done+=(uint64_t)n;
        if ((uint64_t)n<chunk) break;
    }
    free(bounce);
    static unsigned notes;
    if (__atomic_fetch_add(&notes,1,__ATOMIC_RELAXED)<4)
        printf("Runtime: a file read into guest memory failed (a page protected again); read through a host buffer\n");
    return (ssize_t)done;
}
static void note_read_failure(int fd,uint64_t size,int64_t offset,int error) {
    static unsigned noted;
    if (__atomic_fetch_add(&noted,1,__ATOMIC_RELAXED)>=16) return;
    File *f=get(fd);
    printf("Runtime: reading %s (%llu bytes at %lld) failed: %s\n",f ? f->path : "?",
           (unsigned long long)size,(long long)offset,strerror(error));
}
static int64_t do_read(int fd,void *buffer,uint64_t size) {
    int h=host_fd(fd);
    if (h<0) return get(fd) ? -EISDIR : -EBADF;
    runtime_memory_note_write((uintptr_t)buffer,size);
    touch_for_write(buffer,size);
    const int loader=fd_is_loader(fd);
    const uint64_t start=stream_read_begin(loader);
    ssize_t n=read(h,buffer,size);
    if (n<0 && size && runtime_memory_guest_writable((uintptr_t)buffer,size)) n=read_bounced(h,buffer,size,-1);
    stream_read_end(loader,start,n);
    if (n<0) { int e=errno; note_read_failure(fd,size,-1,e); return -e; }
    if (n>0) runtime_memory_note_write((uintptr_t)buffer,(uint64_t)n);
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return get(fd) ? -EISDIR : -EBADF;
    runtime_memory_note_write((uintptr_t)buffer,size);
    touch_for_write(buffer,size);
    const int loader=fd_is_loader(fd);
    const uint64_t start=stream_read_begin(loader);
    ssize_t n=pread(h,buffer,size,offset);
    if (n<0 && size && offset>=0 && runtime_memory_guest_writable((uintptr_t)buffer,size)) n=read_bounced(h,buffer,size,offset);
    stream_read_end(loader,start,n);
    if (n<0) { int e=errno; note_read_failure(fd,size,offset,e); return -e; }
    if (n>0) runtime_memory_note_write((uintptr_t)buffer,(uint64_t)n);
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_write(int fd,const void *buffer,uint64_t size) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    ssize_t n=write(h,buffer,size);
    if (n<0) return -errno;
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pwrite(int fd,const void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    ssize_t n=pwrite(h,buffer,size,offset);
    return n<0 ? -errno : n;
}
static int64_t do_lseek(int fd,int64_t offset,int whence) {
    File *f=get(fd);
    if (!f) return -EBADF;
    if (whence<0 || whence>2) return -EINVAL;
    if (f->dir) {
        if (whence==0 && offset>=0) { f->position=(size_t)offset; return offset; }
        return -EINVAL;
    }
    int64_t r=lseek(f->host,offset,whence);
    return r<0 ? -errno : r;
}
static int64_t do_stat(const char *guest,GuestStat *out);
static int64_t do_fstat(int fd,GuestStat *out) {
    int h=host_fd(fd);
    if (h<0) {
        File *f=get(fd);
        return f && f->dir ? do_stat(f->path,out) : -EBADF;
    }
    if (!out) return -EFAULT;
    HostStat s;
    if (host_fstat(h,&s)) return -errno;
    convert_stat(&s,out); return 0;
}
static int64_t do_stat(const char *guest,GuestStat *out) {
    char path[1024]; HostStat s;
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (!out) return -EFAULT;
    if (host_stat(path,&s)) return -errno;
    convert_stat(&s,out); return 0;
}
static int64_t do_getdents(int fd,char *buffer,uint64_t size,int64_t *basep) {
    host_lock(&lock);
    File *f=get(fd);
    int64_t result=0;
    if (!f) result=-EBADF;
    else if (!f->dir) result=-EINVAL;
    else if (!buffer) result=-EFAULT;
    else if (size<512) result=-EINVAL;
    else {
        if (basep) *basep=(int64_t)f->position;
        uint64_t written=0;
        while (f->position<f->dir->count) {
            const char *name=f->dir->names+f->dir->offsets[f->position];
            size_t n=strlen(name); if (n>255) n=255;
            uint16_t reclen=(uint16_t)((8+n+1+7)&~(size_t)7);
            if (written+reclen>size) break;
            char *p=buffer+written;
            memset(p,0,reclen);
            uint32_t ino=(uint32_t)f->position+1;
            memcpy(p,&ino,4); memcpy(p+4,&reclen,2);
            p[6]=(char)f->dir->types[f->position]; p[7]=(char)n;
            memcpy(p+8,name,n);
            written+=reclen; ++f->position;
        }
        result=(int64_t)written;
    }
    host_unlock(&lock);
    return result;
}
static int64_t path_op(const char *guest,int op,int mode) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    int r= op==0 ? mkdir(path,mode ? mode : 0755) : op==1 ? rmdir(path) : unlink(path);
    return r ? -errno : 0;
}
static int64_t do_rename(const char *from,const char *to) {
    if (game_path(from) || game_path(to)) return -EROFS;
    char a[1024],b[1024];
    int e=translate(from,a,sizeof(a));
    if (!e) e=translate(to,b,sizeof(b));
    if (e) return -e;
    return rename(a,b) ? -errno : 0;
}
static int64_t do_ftruncate(int fd,int64_t length) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    return ftruncate(h,length) ? -errno : 0;
}
static int64_t do_truncate(const char *guest,int64_t length) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    return truncate(path,length) ? -errno : 0;
}
static int64_t do_fsync(int fd) { int h=host_fd(fd); if (h<0) return -EBADF; return fsync(h) ? -errno : 0; }
static int64_t do_access(const char *guest,int mode) {
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
#ifdef _WIN32
    return access(path,mode&6) ? -errno : 0;
#else
    return access(path,mode&7) ? -errno : 0;
#endif
}

static int64_t sce(int64_t r) { return r<0 ? ERR(runtime_guest_errno((int)-r)) : r; }
static int64_t posix(int64_t r) { if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; } return r; }
#define PAIR(name,params,args) \
    static ABI int64_t sce_##name params { return sce(do_##name args); } \
    static ABI int64_t posix_##name params { return posix(do_##name args); }
PAIR(open,(const char *p,int f,int m),(p,f,m))
PAIR(close,(int fd),(fd))
PAIR(read,(int fd,void *b,uint64_t n),(fd,b,n))
PAIR(pread,(int fd,void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(write,(int fd,const void *b,uint64_t n),(fd,b,n))
PAIR(pwrite,(int fd,const void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(lseek,(int fd,int64_t o,int w),(fd,o,w))
PAIR(fstat,(int fd,GuestStat *s),(fd,s))
PAIR(stat,(const char *p,GuestStat *s),(p,s))
PAIR(getdents,(int fd,char *b,uint64_t n,int64_t *base),(fd,b,n,base))
PAIR(rename,(const char *a,const char *b),(a,b))
PAIR(ftruncate,(int fd,int64_t l),(fd,l))
PAIR(truncate,(const char *p,int64_t l),(p,l))
PAIR(fsync,(int fd),(fd))
static ABI int64_t posix_access(const char *p,int m) { return posix(do_access(p,m)); }
static ABI int64_t sce_mkdir(const char *p,int m) { return sce(path_op(p,0,m)); }
static ABI int64_t posix_mkdir(const char *p,int m) { return posix(path_op(p,0,m)); }
static ABI int64_t sce_rmdir(const char *p) { return sce(path_op(p,1,0)); }
static ABI int64_t posix_rmdir(const char *p) { return posix(path_op(p,1,0)); }
static ABI int64_t sce_unlink(const char *p) { return sce(path_op(p,2,0)); }
static ABI int64_t posix_unlink(const char *p) { return posix(path_op(p,2,0)); }
static ABI int32_t sce_check_reachability(const char *p) {
    GuestStat s; return (int32_t)sce(do_stat(p,&s));
}

static const RuntimeExport exports[]={
    {"sceKernelOpen",sce_open}, {"open",posix_open}, {"_open",posix_open},
    {"sceKernelClose",sce_close}, {"close",posix_close}, {"_close",posix_close},
    {"sceKernelRead",sce_read}, {"read",posix_read}, {"_read",posix_read},
    {"sceKernelPread",sce_pread}, {"pread",posix_pread},
    {"sceKernelWrite",sce_write}, {"write",posix_write}, {"_write",posix_write},
    {"sceKernelPwrite",sce_pwrite}, {"pwrite",posix_pwrite},
    {"sceKernelLseek",sce_lseek}, {"lseek",posix_lseek},
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat},
    {"sceKernelGetdirentries",sce_getdents}, {"getdirentries",posix_getdents},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate}, {"truncate",posix_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
    {"access",posix_access}, {"sceKernelCheckReachability",sce_check_reachability},
    {"sceKernelMkdir",sce_mkdir}, {"mkdir",posix_mkdir},
    {"sceKernelRmdir",sce_rmdir}, {"rmdir",posix_rmdir},
    {"sceKernelUnlink",sce_unlink}, {"unlink",posix_unlink},
};
uintptr_t runtime_file_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
int64_t runtime_file_open(const char *p,int f,int m) { return do_open(p,f,m); }
int64_t runtime_file_close(int fd) { return do_close(fd); }
int64_t runtime_file_read(int fd,void *b,uint64_t n) { return do_read(fd,b,n); }
int64_t runtime_file_pread(int fd,void *b,uint64_t n,int64_t o) { return do_pread(fd,b,n,o); }
int64_t runtime_file_write(int fd,const void *b,uint64_t n) { return do_write(fd,b,n); }
int64_t runtime_file_lseek(int fd,int64_t o,int w) { return do_lseek(fd,o,w); }
int64_t runtime_file_stat(const char *p,void *s) { return do_stat(p,s); }
int64_t runtime_file_fstat(int fd,void *s) { return do_fstat(fd,s); }
int64_t runtime_file_getdents(int fd,char *b,uint64_t n,int64_t *base) { return do_getdents(fd,b,n,base); }
int runtime_file_translate(const char *guest,char *out,size_t size) { return translate(guest,out,size); }
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           opens,reads,(unsigned long long)bytes_read,writes,missing);
}
