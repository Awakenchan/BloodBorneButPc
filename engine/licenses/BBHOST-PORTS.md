# bbhost: the Hunter's Dream online backend

Source: https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771

The online backend for The Hunter's Dream adapts bbhost's: `gpu/bbnet/thd/*`, the play-log
redirect in `scripts/online.py` (after bbhost's `src/hle/http.cpp`), and the launcher's sign-in
(`launcher/BloodborneLauncher/Core/HuntersDream.cs`). It is kept as it is so the service's
servers see the client they expect.

These portions are GPL-3.0-or-later. Copyright remains with the bbhost contributors, and
Lantern Engine's additional terms (`NOTICE`) do not apply to them. Builds containing these
portions are distributed under GPL-3.0-or-later (see `GPL-3.0.txt`).
