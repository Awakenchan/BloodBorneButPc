#pragma once
#include <cstddef>
#ifdef TEST_POINTER
bool test_safe_read(const void*,void*,size_t);
namespace BbPlatform {inline bool ReadProcessMemory(const void* p,void* o,size_t n){return test_safe_read(p,o,n);}}
#else
namespace BbPlatform {inline bool ReadProcessMemory(const void*,void*,size_t){return false;}}
#endif
