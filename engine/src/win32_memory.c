/* Windows guest address space. The guest range is reserved at start as one placeholder
 * (VirtualAlloc2); a mapping splits the placeholder and replaces its piece with a view of
 * the pool section (MapViewOfFile3), which allows 4 KiB granularity and aliasing like
 * mmap(MAP_SHARED|MAP_FIXED) of a memfd. Windows cannot unmap part of a view, so views are
 * tracked here: a removed range unmaps the views it touches and maps their remaining pieces
 * again (the section keeps the data). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32_memory.h"

typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL (WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
static VirtualAlloc2Fn virtual_alloc2;
static MapViewOfFile3Fn map_view3;
static UnmapViewOfFile2Fn unmap_view2;
static HANDLE section;
static uintptr_t space_start, space_end;
/* BB_POOL_LAZY=1: the pool section reserves its size and pages are committed as they are mapped,
 * through the pool's own view (every view of a page sees it committed). A committed pool charged
 * Windows its whole size at the start (5056 or 9152 MiB plus 1 GiB of flexible memory), while
 * the game's logs show it touching 1-3 GiB at the start and 2.5-6 GiB in play. */
static int lazy;
static unsigned char *pool_view;
static uint64_t pool_bytes;
typedef struct { uintptr_t start, end; uint64_t phys; } View;
static View *views;
static size_t view_count, view_capacity;

static void report(const char *what, uintptr_t a, uintptr_t b) {
    fprintf(stderr, "Runtime: %s [%#llx, %#llx) failed: Windows error %lu\n", what,
            (unsigned long long)a, (unsigned long long)b, GetLastError());
}
int win_mem_space(uintptr_t start, uintptr_t end) {
    if (space_end) return 0;
    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    virtual_alloc2 = kernel ? (VirtualAlloc2Fn)(void *)GetProcAddress(kernel, "VirtualAlloc2") : NULL;
    map_view3 = kernel ? (MapViewOfFile3Fn)(void *)GetProcAddress(kernel, "MapViewOfFile3") : NULL;
    unmap_view2 = kernel ? (UnmapViewOfFile2Fn)(void *)GetProcAddress(kernel, "UnmapViewOfFile2") : NULL;
    if (!virtual_alloc2 || !map_view3 || !unmap_view2) {
        fputs("STOP: Windows 10 1803 or newer is required (placeholder memory API)\n", stderr);
        exit(21);
    }
    if (virtual_alloc2(GetCurrentProcess(), (void *)start, end - start, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                       PAGE_NOACCESS, NULL, 0) != (void *)start) {
        report("reserving the guest address space", start, end);
        return -1;
    }
    space_start = start; space_end = end;
    return 0;
}
int win_mem_section(uint64_t size, void **backing) {
    const char *lazy_env = getenv("BB_POOL_LAZY");
    lazy = lazy_env && lazy_env[0] == '1';
    pool_bytes = size;
    /* bbport: bb-start made this section (its commit charged there) and passes it on as an
     * inherited handle, "handle:bytes", so the size it was checked at is the size the game gets. */
    const char *given = getenv("BB_POOL_SECTION");
    if (given && *given) {
        char *end = NULL;
        const unsigned long long handle = strtoull(given, &end, 10);
        const unsigned long long bytes = end && *end == ':' ? strtoull(end + 1, NULL, 10) : 0;
        if (handle && bytes == size) {
            section = (HANDLE)(uintptr_t)handle;
            *backing = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size);
            pool_view = *backing;
            if (*backing) return 0;
            fprintf(stderr, "Runtime: the pool bb-start made cannot be mapped (Windows error %lu); making one\n",
                    GetLastError());
        } else {
            fprintf(stderr, "Runtime: BB_POOL_SECTION %s is not a %llu byte pool; making one\n", given,
                    (unsigned long long)size);
        }
        section = NULL; /* a stale handle (another process's value) is not ours to close */
    }
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                 PAGE_EXECUTE_READWRITE | (lazy ? SEC_RESERVE : SEC_COMMIT),
                                 (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!section) { report("creating the memory pool section", 0, size); return -1; }
    *backing = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!*backing) { report("mapping the memory pool", 0, size); return -1; }
    pool_view = *backing;
    if (lazy) {
        fprintf(stderr, "Runtime: the game's memory (%llu MiB) is committed as it is mapped "
                        "(BB_POOL_LAZY=1)\n", (unsigned long long)(size >> 20));
    }
    return 0;
}
int win_mem_commit(uint64_t phys, uint64_t size) {
    if (!lazy || !pool_view || !size || phys >= pool_bytes) return 0;
    if (size > pool_bytes - phys) size = pool_bytes - phys;
    if (VirtualAlloc(pool_view + phys, size, MEM_COMMIT, PAGE_READWRITE)) return 0;
    fprintf(stderr, "Runtime: Windows has no memory left to commit for the game's %llu MiB at %#llx "
                    "(error %lu): close other programs or let Windows grow the page file\n",
            (unsigned long long)(size >> 20), (unsigned long long)phys, GetLastError());
    return -1;
}
void win_mem_zero(uint64_t phys, uint64_t size) {
    if (!lazy) { memset(pool_view + phys, 0, size); return; }
    MEMORY_BASIC_INFORMATION info;
    for (uint64_t at = phys, end = phys + size; at < end;) {
        if (!VirtualQuery(pool_view + at, &info, sizeof(info))) return;
        uint64_t next = (uint64_t)((unsigned char *)info.BaseAddress + info.RegionSize - pool_view);
        if (next > end) next = end;
        if (info.State == MEM_COMMIT) memset(pool_view + at, 0, next - at);
        at = next;
    }
}
static DWORD page_mode(int prot) {
    switch (prot & 7) {
    case 0: return PAGE_NOACCESS;
    case 1: return PAGE_READONLY;
    case 2: case 3: return PAGE_READWRITE;
    case 4: return PAGE_EXECUTE;
    case 5: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}
static size_t view_index(uintptr_t a) { /* first view with end > a */
    size_t lo = 0, hi = view_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (views[mid].end <= a) lo = mid + 1; else hi = mid; }
    return lo;
}
static int view_insert(View v) {
    if (view_count == view_capacity) {
        size_t capacity = view_capacity ? view_capacity * 2 : 256;
        View *next = realloc(views, capacity * sizeof(*views));
        if (!next) return -1;
        views = next; view_capacity = capacity;
    }
    size_t at = view_index(v.start);
    memmove(views + at + 1, views + at, (view_count - at) * sizeof(*views));
    views[at] = v; ++view_count;
    return 0;
}
static void view_erase(size_t at) {
    memmove(views + at, views + at + 1, (view_count - at - 1) * sizeof(*views));
    --view_count;
}
/* Makes x a placeholder boundary. Placeholder pieces report their own AllocationBase. */
static int split_at(uintptr_t x) {
    if (x == space_start || x == space_end) return 0;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)x, &info, sizeof(info))) return -1;
    if ((uintptr_t)info.AllocationBase == x) return 0;
    if (info.State != MEM_RESERVE) { SetLastError(ERROR_INVALID_ADDRESS); return -1; }
    return VirtualFree((void *)x, info.RegionSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
}
/* [a,b) holds placeholders only: make it exactly one placeholder. */
static int make_placeholder(uintptr_t a, uintptr_t b) {
    if (split_at(a) || split_at(b)) return -1;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)a, &info, sizeof(info))) return -1;
    if (info.RegionSize < b - a && !VirtualFree((void *)a, b - a, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) return -1;
    return 0;
}
static int map_view(uintptr_t a, uintptr_t b, uint64_t phys) {
    if (make_placeholder(a, b)) return -1;
    if (map_view3(section, GetCurrentProcess(), (void *)a, phys, b - a, MEM_REPLACE_PLACEHOLDER,
                  PAGE_READWRITE, NULL, 0) != (void *)a) return -1;
    return view_insert((View){a, b, phys});
}
static int inside(uintptr_t a, uintptr_t b) { return a < b && a >= space_start && b <= space_end; }
int win_mem_release(uintptr_t a, uintptr_t b, WinMemRestore restore) {
    if (!inside(a, b)) { SetLastError(ERROR_INVALID_ADDRESS); report("releasing", a, b); return -1; }
    for (size_t i = view_index(a); i < view_count && views[i].start < b;) {
        View v = views[i];
        if (!unmap_view2(GetCurrentProcess(), (void *)v.start, MEM_PRESERVE_PLACEHOLDER)) { report("unmapping", v.start, v.end); return -1; }
        view_erase(i);
        if (v.start < a) {
            if (map_view(v.start, a, v.phys)) { report("remapping", v.start, a); return -1; }
            if (restore) restore(v.start, a);
            ++i;
        }
        if (v.end > b) {
            if (map_view(b, v.end, v.phys + (b - v.start))) { report("remapping", b, v.end); return -1; }
            if (restore) restore(b, v.end);
        }
    }
    if (make_placeholder(a, b)) { report("reserving", a, b); return -1; }
    return 0;
}
int win_mem_map(uintptr_t a, uintptr_t b, uint64_t phys, int prot, WinMemRestore restore) {
    /* BB_POOL_LAZY: committed before it is mapped, so protections and Windows' own reads into
     * the mapping (file reads) find every page there. */
    if (win_mem_commit(phys, b - a)) { SetLastError(ERROR_COMMITMENT_LIMIT); return -1; }
    if (win_mem_release(a, b, restore)) return -1;
    if (map_view(a, b, phys)) { report("mapping", a, b); return -1; }
    return (prot & 7) == 3 ? 0 : win_mem_protect(a, b, prot);
}
/* VirtualProtect cannot cross views: one call per view. */
int win_mem_protect(uintptr_t a, uintptr_t b, int prot) {
    DWORD mode = page_mode(prot), old;
    for (size_t i = view_index(a); i < view_count && views[i].start < b; ++i) {
        uintptr_t start = views[i].start > a ? views[i].start : a, end = views[i].end < b ? views[i].end : b;
        if (!VirtualProtect((void *)start, end - start, mode, &old)) { report("protecting", start, end); return -1; }
    }
    return 0;
}
void *win_mem_private(uintptr_t a, uintptr_t b) {
    if (!inside(a, b)) return NULL;
    MEMORY_BASIC_INFORMATION info;
    /* Taken pieces (earlier private allocations, views) are skipped by the caller's search. */
    for (uintptr_t at = a; at < b; at = (uintptr_t)info.BaseAddress + info.RegionSize)
        if (!VirtualQuery((void *)at, &info, sizeof(info)) || info.State != MEM_RESERVE) return NULL;
    if (make_placeholder(a, b)) return NULL;
    return virtual_alloc2(GetCurrentProcess(), (void *)a, b - a, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                          PAGE_READWRITE, NULL, 0);
}
#endif
