// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace BbHeapSites {
void Install();
void Report();
void NoteReleaseCheck(unsigned result);
void InstallCounters();
bool Counting();
long long LiveAllocations();
}
