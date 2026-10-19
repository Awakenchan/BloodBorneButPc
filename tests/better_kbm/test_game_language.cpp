// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_language.h"
#include <cassert>
#include <vector>
#include <algorithm>
#include <utility>
#include <cstdio>
int main() {
    using namespace GameLanguage;
    std::size_t size=0;for(const auto& site:sites)size=std::max(size,site.at+5);
    std::vector<std::uint8_t> image(size);
    for(const auto& site:sites)std::memcpy(image.data()+site.at,site.expected.data(),5);
    const auto before=image;
    auto writer=[](void* at,const void* bytes,std::size_t n){std::memcpy(at,bytes,n);return true;};
    for(auto [language,index]:{std::pair{0,0},{1,1},{2,2},{3,5},{4,4},{5,3},{17,7},{20,6}}) {
        image=before;assert(Apply(image.data(),size,language,writer));
        for(const auto& site:sites) {
            assert(image[site.at]==0xb8&&image[site.at+1]==index);
            for(int i=2;i<5;++i)assert(image[site.at+i]==0);
        }
        // Only voice property/conversion results change; text-language code is untouched.
        for(const auto& site:sites)std::memcpy(image.data()+site.at,site.expected.data(),5);
        assert(image==before);
    }
    image=before;image[sites[1].at]=0;const auto invalid=image;int writes=0;
    assert(!Apply(image.data(),size,1,[&](void*,const void*,std::size_t){++writes;return true;}));
    assert(writes==0&&image==invalid);
    image=before;
    assert(!Apply(image.data(),size,2,[&](void* at,const void* bytes,std::size_t n){
        std::memcpy(at,bytes,n);return ++writes!=2;
    }));
    assert(image==before);
    assert(Parse("20")==20&&Parse("17")==17&&Parse("0")==0&&Parse("8")==-1&&Parse("17oops")==-1&&Parse(nullptr)==-1);
    assert(std::strcmp(TrackName(20),"esar")==0&&std::strcmp(TrackName(3),"eses")==0);
    assert(VoiceIndex(20)==6&&VoiceIndex(3)==5);
    puts("PASS: all eight platform-to-native voice mappings, distinct esar/eses tracks, regional override, no text changes, signature rejection and rollback.");
}
