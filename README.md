# Combat Group Deadlock Fix

An SKSE plugin for Skyrim AE 1.6.1170 that fixes a vanilla engine freeze.

**The freeze:** starting combat locks a combat group for writing and then waits on another actor's AI data. That actor's update holds its AI data and asks whether someone is a target of the same combat group (Address Library ID 44706). Each waits for the other forever, and the game freezes.

**The fix:** ID 44706 is replaced by the same check, made the same way, except that it gives up waiting for the group's lock after half a second and answers "not a target" for that one check. Every patched byte is checked first; if anything differs, the plugin does nothing.

Traced in game on 2026-10-09 with a debugger snapshot of the frozen process. Full notes are at the top of `src/main.cpp`.

## Build
```
xmake f -P . -y -p windows -a x64 -m release --skyrim_ae=y --skyrim_se=n --skyrim_vr=n
xmake build -y
```
After a clone, fill the submodule first with `git submodule update --init --recursive`.

Release: `scripts\package.ps1` builds and zips the mod into `dist/`. Technical notes: [docs/TECHNICAL.md](docs/TECHNICAL.md). Nexus page text: [docs/NEXUS.txt](docs/NEXUS.txt).
