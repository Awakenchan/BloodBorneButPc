#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int done;
    HostCond event;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    char name[32];
    Waiter *first;
    struct Sema *next;
} Sema;
static HostMutex lock=HOST_MUTEX_INIT;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;
static uint64_t lookups, lookup_steps;
static Sema *find_ordered(uint32_t id,int reorder) {
    static int front=-1;
    if (front<0) { const char *e=getenv("BB_SEMA_FRONT"); front=!(e && e[0]=='0'); }
    ++lookups;
    Sema **link=&registry;
    for (Sema *s=registry;s;link=&s->next,s=s->next) {
        ++lookup_steps;
        if (s->id!=id) continue;
        if (reorder && front && link!=&registry) { *link=s->next; s->next=registry; registry=s; }
        return s;
    }
    return NULL;
}
static Sema *find(uint32_t id) { return find_ordered(id,1); }
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
static uint64_t now_ns(void) { return host_monotonic_ns(); }
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    host_lock(&lock);
    if (next_id==UINT32_MAX) { host_unlock(&lock); free(s); return ERR(28); }
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    snprintf(s->name,sizeof s->name,"%s",name);
    s->next=registry; registry=s; *out=s->id; ++created;
    printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",s->id,attr,initial,maximum);
    host_unlock(&lock);
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    host_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        if (getenv("BB_TRACE_SEMA")) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count);
        Waiter w={.need=need};
#ifdef _WIN32
        host_cond_init(&w.event);
#else
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
#endif
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        const uint64_t wait_start=runtime_wait_clock();
        const int slot=runtime_block_begin(RUNTIME_BLOCK_SEMA,id);
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        while (!w.done) {
#ifdef _WIN32
            int e=0;
            if (timeout) {
                uint64_t now=now_ns();
                e=now<deadline ? host_cond_timedwait(&w.event,&lock,deadline-now) : ETIMEDOUT;
            } else host_cond_wait(&w.event,&lock);
#else
            struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
#endif
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        runtime_block_end(slot);
        runtime_wait_note(2,runtime_wait_clock()-wait_start);
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
#ifndef _WIN32
        host_check(pthread_cond_destroy(&w.event));
#endif
        if (!--s->active && s->deleted) free(s);
    }
    host_unlock(&lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) { return wait_count(id,need,timeout,1); }
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    host_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_cond_signal(&w->event);
        }
    }
    host_unlock(&lock);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_cond_signal(&w->event);
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    host_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    host_unlock(&lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    host_lock(&lock);
    Sema **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { host_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    host_unlock(&lock);
    return 0;
}
void runtime_sema_describe(uint32_t id,char *out,size_t size) {
    host_lock(&lock);
    Sema *s=find_ordered(id,0);
    if (!s) snprintf(out,size,"semaphore %u (deleted)",id);
    else {
        int n=snprintf(out,size,"semaphore %u '%s', count %d of %d, waiters needing",id,s->name,s->count,s->maximum);
        for (Waiter *w=s->first;w && n>0 && (size_t)n<size;w=w->next) n+=snprintf(out+n,size-(size_t)n," %d",w->need);
    }
    host_unlock(&lock);
}
unsigned runtime_sema_waiters(uint32_t id) {
    host_lock(&lock);
    Sema *s=find_ordered(id,0); unsigned n=0;
    if (s) for (Waiter *w=s->first;w;w=w->next) ++n;
    host_unlock(&lock);
    return n;
}
uintptr_t runtime_sema_resolve(const char *name) {
    if (!strcmp(name,"188x57JYp0g#p#J")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#p#J")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#p#J")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"12wOHk8ywb0#p#J")) return (uintptr_t)sem_poll;
    if (!strcmp(name,"4DM06U2BNEY#p#J")) return (uintptr_t)sem_cancel;
    if (!strcmp(name,"R1Jvn8bSCW8#p#J")) return (uintptr_t)sem_delete;
    return 0;
}
void runtime_sema_census(uint64_t *live, uint64_t *found, uint64_t *steps) {
    host_lock(&lock);
    *live=created-deleted; *found=lookups; *steps=lookup_steps;
    host_unlock(&lock);
}
void runtime_sema_report(void) {
    host_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    host_unlock(&lock);
}
