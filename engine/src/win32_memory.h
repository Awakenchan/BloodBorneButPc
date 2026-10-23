#ifndef BB_WIN32_MEMORY_H
#define BB_WIN32_MEMORY_H
#ifdef _WIN32
#include <stdint.h>
int win_mem_space(uintptr_t start, uintptr_t end);
int win_mem_section(uint64_t size, void **backing);
int win_mem_commit(uint64_t phys, uint64_t size);
void win_mem_zero(uint64_t phys, uint64_t size);
typedef void (*WinMemRestore)(uintptr_t start, uintptr_t end);
int win_mem_map(uintptr_t a, uintptr_t b, uint64_t phys, int prot, WinMemRestore restore);
int win_mem_release(uintptr_t a, uintptr_t b, WinMemRestore restore);
int win_mem_protect(uintptr_t a, uintptr_t b, int prot);
void *win_mem_private(uintptr_t a, uintptr_t b);
#endif
#endif
