#pragma once
#include <span>
#include <cstdint>
#include <type_traits>
namespace BbEngine {using Callback = std::int64_t (__attribute__((sysv_abi)) *)(std::uint64_t,const std::uint64_t*);
struct Hook{};inline bool Prepare(Hook&,uint8_t*,size_t,size_t,std::span<const uint8_t>,Callback){return true;}
inline void Commit(std::span<Hook>){}
template<class T>int64_t Argument(T v){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<int64_t>(v);else return static_cast<int64_t>(v);}
}
#ifdef TEST_POINTER
int64_t test_guest_call(void*,const int64_t*,size_t);
namespace BbEngine {template<class... A>int64_t Call(uint64_t fn,A... a){const int64_t v[]={Argument(a)...};return test_guest_call(reinterpret_cast<void*>(fn),v,sizeof...(A));}}
#else
namespace BbEngine {template<class... A>int64_t Call(uint64_t,A...){return 0;}}
#endif
