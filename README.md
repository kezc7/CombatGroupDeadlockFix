# Combat Group Deadlock Fix

An SKSE plugin for **Skyrim AE 1.6.1170** that fixes a vanilla engine freeze (a deadlock) that can happen as a fight breaks out.

**Status: new.** The lock code and the replaced function have been verified against the 1.6.1170 code, and the lock is tested against a model of the game's lock. The freeze was traced in a real game; reports of the fix stepping in are welcome. See [docs/TECHNICAL.md](docs/TECHNICAL.md#in-game-test-plan).

## The freeze

Starting combat locks a combat group for writing, then waits for another actor's AI data. That actor's update holds its AI data while it looks for hostile actors. For each candidate, it asks whether that actor is a target of the same combat group (Address Library ID 44706), which needs the group's lock. Each side waits for the other forever.

## The fix

ID 44706 is replaced by the same check, made the same way, with one difference: it stops waiting for the group's lock after half a second and answers "not a target" for that check.
- While the same stuck writer still holds the lock, the same thread answers at once, so the rest of the update doesn't wait again.
- In normal play the lock is held for far less than half a second, so the check behaves as before.
- Before patching, the plugin checks every byte of 44706 and of the game's lock functions. If anything differs, it changes nothing.

## Requirements

- Skyrim Special Edition / Anniversary Edition **1.6.1170** exactly (not 1.5.97, 1.6.640, 1.7.x or VR)
- [SKSE64](https://skse.silverlock.org/) for 1.6.1170
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (All in one, AE)

## Build

```
git submodule update --init --recursive
xmake f -P . -y -p windows -a x64 -m release --skyrim_ae=y --skyrim_se=n --skyrim_vr=n
xmake build CombatGroupDeadlockFix
xmake build locktest && xmake run locktest
```

Release: `powershell -ExecutionPolicy Bypass -File scripts\package.ps1` builds, runs the lock test and zips the mod into `dist/`.

## Documents

- [docs/TECHNICAL.md](docs/TECHNICAL.md): the deadlock, the game's lock as verified, the replacement, risks, alternatives and the test plan
- [CHANGELOG.md](CHANGELOG.md)

## Licence

GPL-3.0-or-later with the Modding Exception: see [LICENSE](LICENSE) and [EXCEPTIONS.md](EXCEPTIONS.md). These are the same terms as [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG), which Combat Group Deadlock Fix is built with. Source: https://github.com/kezc7/CombatGroupDeadlockFix
