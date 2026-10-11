# Combat Group Deadlock Fix: technical notes

Skyrim AE 1.6.1170. Every address below is an Address Library ID.

## The deadlock

The deadlock was taken from a debugger snapshot of a frozen game on 2026-10-09, and the code was then read from the running game.

**Thread A starts combat.**
- `Actor::StartCombat` (`38561`) takes the global combat lock and a combat group's `BSReadWriteLock` at `CombatGroup + 0x160` for **writing**.
- It then goes through `33228 -> 44704 -> 37759 -> 39515` to add an entry to another actor's knowledge list.
- That needs the actor's AI-data lock (`MiddleHighProcessData + 0x238`) for **writing**.

**Thread B updates that same actor** (`39578`).
- It holds the AI-data lock for **reading** while it walks the knowledge list.
- For each entry it goes through `27293 -> 37534 -> 38571` to `44706`, which asks whether an actor is one of the group's targets.
- `44706` takes `CombatGroup + 0x160` for **reading**.

Each thread waits on a lock the other holds, and `BSReadWriteLock` waits without a timeout. The main thread then waits on both, so the process hangs.

## The fix

`44706` is replaced in full by `IsGroupTarget`, using `SKSE::Trampoline::write_branch<5>` at the function's start. The replacement does what the game's code does:
1. gets the actor's handle through `37476`, as the original calls it;
2. read-locks `CombatGroup + 0x160`;
3. scans the target array (data at `+0x08`, count at `+0x18`, a stride of `0xA8` with the handle first);
4. unlocks, as `68239` does.

The one difference is in the read-lock. It reproduces `LockForRead` (`68233`):
- a nested read when the calling thread is the writer;
- otherwise a CAS on the reader count, spinning with `Sleep(0)` and then `Sleep(1)`, as the game does.

Unlike the game's, it gives up after **500 ms**. On a give-up:
- the check returns `false` ("not a target") for that call only;
- the same thread then gives up immediately on the same lock for the next 2 seconds, because thread B can ask once per knowledge entry, so the hitch stays short;
- each give-up is logged (the first 50, then every 100th).

In normal play the group lock is held for far less than that, so the timeout shouldn't fire.

## Install checks

Before patching, `44706` is checked where its behaviour depends on the layout, as read from 1.6.1170:
- the prologue `40 57 48 83 EC 30`;
- `lea rbx, [rcx+0x160]` at `+0x1F`;
- the calls at `+0x2E` (to `68233`) and `+0x3C` (to `37476`);
- `cmp [rsi+0x18], edx` at `+0x47`;
- `imul rcx, rax, 0xA8` at `+0x62`.

If any check fails, nothing is patched and the plugin logs why. Nothing is serialized.

## Behavioural difference

Only during the deadlock: for up to about 2.5 seconds, one actor's knowledge update may see one combat-group target as "not a target" once. The next AI update corrects it.

## Build

```
git submodule update --init --recursive
xmake f -P . -y -p windows -a x64 -m release --skyrim_ae=y --skyrim_se=n --skyrim_vr=n
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```
