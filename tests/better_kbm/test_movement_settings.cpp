// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_settings.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main() {
    auto& settings=BbSettings::Get();
    assert(settings.stick_turn_rate==1200&&settings.stick_release_delay_ms==60&&settings.stick_circular);
    auto path=std::filesystem::temp_directory_path()/
        ("bbport-movement-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".ini");
    const auto file=path.string();
#ifdef _WIN32
    _putenv_s("BB_CONFIG",file.c_str());
#else
    setenv("BB_CONFIG",file.c_str(),1);
#endif
    {
        std::ofstream out(path);
        out<<"bind.roll=Space\nstick_turn_rate=1200\nstick_release_delay_ms=40\nstick_circular=on\n";
    }
    BbSettings::Load();
    assert(settings.stick_turn_rate==1200&&settings.stick_release_delay_ms==40&&settings.stick_circular);
    settings.stick_turn_rate=900;settings.stick_release_delay_ms=60;settings.stick_circular=false;
    BbSettings::Save();
    {
        std::ifstream in(path);
        const std::string saved{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
        for (auto expected : {"bind.roll=Space\n","stick_turn_rate=900\n","stick_release_delay_ms=60\n","stick_circular=0\n"})
            assert(saved.find(expected)!=std::string::npos);
    }
    settings.stick_turn_rate=1;settings.stick_release_delay_ms=2;settings.stick_circular=true;
    BbSettings::Load();
    assert(settings.stick_turn_rate==900&&settings.stick_release_delay_ms==60&&!settings.stick_circular);
    {
        std::ofstream out(path);
        out<<"stick_turn_rate=-20\nstick_release_delay_ms=9999\nstick_circular=off\n";
    }
    BbSettings::Load();
    assert(settings.stick_turn_rate==0&&settings.stick_release_delay_ms==200&&!settings.stick_circular);
    std::filesystem::remove(path);
    std::cout<<"PASS: movement defaults, loading, save/reload, limits and preserved key bindings.\n";
}
