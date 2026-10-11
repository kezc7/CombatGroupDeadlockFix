# Combat Group Deadlock Fix: technical notes

> Version names: "1.0" in this document is the private development build of 2026-10-09 and "1.1" is the audited code, released publicly as **1.0.0**.

Skyrim AE **1.6.1170** only (`SkyrimSE.exe` 1.6.1170.0). Every number written as an ID is an Address Library ID; RVAs are offsets from the start of the executable.

## How this was established

Everything below comes from the 1.6.1170 machine code, not from names or comments.
- The executable's code section is encrypted on disk (SteamStub). It was decrypted locally, for reading only: the stub header's `0xC0DEC0DF` signature, AES key and IV were used, and the result was confirmed by every function disassembling into clean, `int3`-padded code.
- The result matches, byte for byte, every pattern 1.0 had read from the running game on 2026-10-09.
- IDs were resolved with the Address Library database `versionlib-1-6-1170-0.bin`. Imports were resolved through the executable's import table: `175063` is `KERNEL32!GetCurrentThreadId`, and `175091` is `KERNEL32!Sleep`.
- The code the plugin depends on is fingerprinted in [`src/GameFingerprints.h`](../src/GameFingerprints.h): each function's length and FNV-1a 64 hash. Install hashes the live game and patches only on a match. None of those ranges contain base relocations, so the fingerprints are the same wherever the game loads. No game code is included.

## The deadlock

The deadlock was first seen in a debugger snapshot of a frozen game (2026-10-09). Every call edge below has since been confirmed in the 1.6.1170 code as a direct call or tail jump.

**Thread A starts combat.**
- `38561` (StartCombat) → `33228` → `44704` → `37759` → `39515`.
- Thread A holds the combat group's lock (`CombatGroup + 0x160`) for **writing**.
- `44704` also read-locks the same group, nested under the write.
- `39515` then calls `LockForWrite` (`68234`) on the other actor's AI-data lock (`+0x238`) to add a knowledge entry.

**Thread B updates that actor.**
- `39578` calls `LockForRead` (`68233`) on the same AI-data lock (`+0x238`).
- It then loops over the actor's knowledge entries (16-byte entries at `+0x220`), calling `27293` for each one. It stops at the first hostile actor it finds, then unlocks.
- `27293` → `37533` → `37534` ("is this actor hostile to that one") → `38571` (that actor's combat group, then `44706`).
- `44706` calls `LockForRead` on the group's lock.

Thread A waits for B's read lock to go, and B waits for A's write lock to go. Neither lock function has a timeout, so the game hangs.

## The game's `BSReadWriteLock` (verified)

| | |
|---|---|
| Layout | `+0 writerThread` (u32), `+4 count` (u32); 8 bytes. Matches CommonLibSSE-NG `RE::BSReadWriteLock`. |
| `count` | `0x80000000` while written, plus 1 per hold. A writer's first hold makes `0x80000001`; its nested reads and writes add to that. Otherwise it is the number of readers. |
| `LockForRead` (`68233`) | If `writerThread == GetCurrentThreadId()`: `lock inc count` (nested). Otherwise loop: `expected = count & 0x0FFFFFFF`, `lock cmpxchg count, expected+1`. A failed swap that saw a value `>= 0` retries at once (another reader moved the count). One that saw a value `< 0` (writer bit) calls `Sleep(0)` for 10,000 tries, then `Sleep(1)`, re-reads and retries. |
| `LockForWrite` (`68234`) | If already the writer: `lock inc count`. Otherwise loop `lock cmpxchg count, 0x80000001` expecting **0**, sleeping as above. Then `writerThread = tid; mfence`. |
| `UnlockForRead` (`68239`) | `lock dec count`, in every state. Nothing else. |
| `UnlockForWrite` (`68240`) | If `count == 0x80000001`: `writerThread = 0; mfence; xchg count, 0`. Otherwise `lock dec count`. |

Consequences:
- **The mask:** `0x0FFFFFFF` is exactly what `LockForRead` uses. A reader gets in only when bits 28..31 are all clear, so never while a writer holds the lock.
- **No pending-writer state:** a writer is only ever "in" (bit set, after a successful swap from 0) or "out". Readers can therefore always get in ahead of a waiting writer and can starve it. That is the game's design; this plugin neither adds to it nor removes it.
- **Recursion:** readers may nest (the count just rises; `44704` relies on this). The writer may take nested reads and writes.
- **Ordering:** every change to `count` is a lock-prefixed instruction, which is a full barrier on x64. Data read under the lock can't move outside it.
- **Transient states:** `writerThread` is set after the writer bit and cleared before it. For a moment, a written lock can show `writerThread == 0`.
- **Counter edges:** at `0x0FFFFFFF` readers, `LockForRead` makes `0x10000000`, and any later reader then spins forever. That needs 2^28 holds, so it is unreachable in practice.

## ID 44706 (verified)

The full function (`0xA6` bytes):
1. `lea rbx,[rcx+0x160]`; then `LockForRead`.
2. The actor's handle, via `37476(actor, &out)`. This returns `&out` and writes the invalid handle for a null or unreferenced actor.
3. A scan of `count = [group+0x18]` entries, each `0xA8` bytes, from `[group+0x08]`, comparing each entry's first dword to the handle.
4. `UnlockForRead`.
5. `movzx eax, dil`: it returns 0 or 1 in all of `eax`.

This matches CommonLibSSE-NG:
- `CombatGroup::targets` is a `BSTArray<CombatTarget>` at `+0x08`, so its data is at `+0x08` and its size at `+0x18`.
- `sizeof(CombatTarget) == 0xA8`, with `targetHandle` at `+0x00`.
- `CombatGroup::lock` is at `+0x160`.

Like the game's function, the replacement doesn't check for a null group. `38571` checks before calling.

**Callers.**
- 11 direct calls: `37766, 38626, 40590, 41240, 41260, 41261, 44704, 44711, 44756, 46876, 46888`.
- 2 tail jumps: `38560` (a virtual function in two vtables) and `38571`.
- `38571` itself has 21 callers.
- All 32 sites that can be found statically test only `al`. The callers of the virtual `38560` can't be listed statically. The replacement returns 0/1 in all of `eax`, as the game's does, so how a caller reads it doesn't matter.
- `44704` already holds a read lock on the same group when it calls `44706`. While any read is held no writer can get in, so that call can never time out.

## The replacement

`44706` is replaced in full: `write_branch<5>` at its first byte jumps to `IsGroupTarget` ([`src/main.cpp`](../src/main.cpp)). That function does the same five steps, in the same order, with the same reads.

The lock is taken by `LockForReadWithin` ([`src/GameLock.h`](../src/GameLock.h)). It follows `LockForRead` step for step: the nested path, the same mask and swap, an immediate retry when another reader moved the count, and `Sleep(0)`×10000 then `Sleep(1)` while a writer holds it. It differs in two places only:
1. **Waiting for a writer stops after a time limit.** On a give-up the function returns 0 ("not a target") without touching the lock.
2. A count with bits 28..30 set and bit 31 clear is treated as a writer, so the wait ends at the limit. The game would spin forever on it, but its code never makes that state.

Unlocking is `lock dec count`, the whole of `UnlockForRead`.

`tests/locktest` (below) shows that it leaves the lock word exactly as the modelled game lock does, in every state tested.

### The give-up policy

`LockGroupForRead` applies the policy: `giveUpAfterMs = 500`, `quickAnswerForMs = 2000`, `quickGapMs = 50`.

**500 ms wait.**
- In the deadlock, the write hold never ends, so the wait length is the hitch.
- Outside it, a write hold of 500 ms is already a visible stall. In the traced paths, a writer holds the group lock only while it does bounded work.
- Shorter waits make a false "not a target" during a legitimate long hold more likely (for example, a preempted writer or a loading spike). Longer waits lengthen the hitch.
- 500 ms is a judgement, not a measurement. Waits of 100 ms or more that do get the lock are logged, so in-game logs can show how close normal play comes to the limit.

**Quick give-ups.**
- After one full give-up, thread B is usually still in its knowledge walk. It still holds the AI-data lock, and it will ask again, up to twice per entry, about the same group. A full wait per entry would turn a 0.5 s hitch into several seconds.
- So the same thread gives up at once on the same lock while all of these hold:
  - the lock is still written by the same thread;
  - this thread has asked again within 50 ms of its last give-up. A walk asks back to back, and 50 ms is about three `GetTickCount64` ticks.
  - less than 2 s have passed since the full wait.
- A quick give-up still tries the lock once, so a lock that has come free is taken.
- Taking the lock forgets the give-up. Asks about other groups (the walk visits many actors) leave it alone.
- A thread id alone can't tell two holds by the same thread apart. Within the gap, a new hold by the same writer could still get a quick answer; the pause rule is what ends the reuse.

**Changes from 1.0.**
- In 1.0, the fast-fail state was the lock address plus 2 s. Its fast-fail mode also counted a lost race with another *reader* as a failure. With only readers around, it answered "not a target" on 14–48% of tries in `locktest`.
- Its state also applied to any later short hold, by any writer, and to a new group allocated at the same address. Those answers were never logged.

## What a "not a target" answer can do

The answer is "not a target" (0) only when the group's lock was write-held without a break for the full wait, or during a quick give-up as described above.

- **On the deadlock path:** `37534` treats "is a target of that actor's combat group" as *hostile* (`+0x195`). With 0, it falls through to its other checks (faction relations and so on). It may still find the actor hostile, or it may not. `27293` then doesn't pick that actor in this walk; it keeps looking or finds nothing.
  - That is the result of one search. It doesn't write AI state itself, and the next update searches again.
  - What `39578` does with "found nobody" this once was not traced further.
- **Elsewhere:** 44706 has 33 or more consumers. Most were not traced to their effects. Any of them gets 0 only after a 500 ms or longer write hold, which in vanilla would have been a 500 ms or longer stall of that thread.
  - It can't be ruled out that a single 0 there changes some persistent state. Examples would be a target-selection or combat-music decision, or a choice not to join or keep fighting.
  - This is the main behavioural risk. It needs in-game validation; see the test plan.
- **Diagnosis:** every full give-up is logged (the first 20, then every 100th), with the writing thread's id and a running count of quick give-ups. A give-up means a write hold of 500 ms or more. That is most likely this deadlock, but the log can't prove it.

## Alternatives considered

| Approach | Verdict |
|---|---|
| Fix the lock order: thread A takes the AI-data lock before the group lock | Needs `StartCombat`'s locking rewritten across `38561`/`33228`/`44704`. The global combat lock is also involved. Large, invasive, and needs the full lock graph. **Rejected.** |
| Thread A: try the AI-data write lock in `39515` and skip on failure | Skipping drops a knowledge entry, which is persistent state. Worse than a transient query answer. **Rejected.** |
| Thread A: defer the knowledge write until the group lock is released | Needs a queue, its own lifetime handling for the actors involved, and a guarantee that nothing reads the entry in the meantime. **Rejected** as too risky without a full model. |
| Thread B: release the AI-data read lock around the hostility check | The walk iterates a container that lock protects. Releasing it mid-walk risks iterating a changed container: a memory-safety risk. **Rejected.** |
| Only hook the `38571` call sites in `37534` | Narrower, so other callers keep vanilla behaviour. But any other reader that holds an AI-data lock and calls `44706` would still deadlock, and the hooks would sit at three call sites instead of one entry point. **Not chosen.** The global replacement only behaves differently after a 500 ms write hold. |
| **Bounded read-lock in `44706` (kept)** | Breaks the cycle on the side whose output is one transient query. Leaves every lock state as the game's lock would. Fingerprint-checked at install. |

## Install checks

All of these must pass, or nothing is patched and each failed check is logged:
- SKSE loads the plugin only on 1.6.1170. It declares that one runtime as compatible, with no Address Library version independence. `SKSEPlugin_Load` checks the runtime again before anything else.
- All `0xA6` bytes of `44706` match.
- Its three calls go to `68233`, `37476` and `68239`.
- All bytes of `68233`, `68234`, `68239` and `68240` match. The replacement shares their protocol, so if another mod changed any of them, the plugin must not install.

Nothing is written to saves. Another plugin that patches `44706` after this one will see a `jmp`, and its own checks decide what it does.

## Tests

### `tests/locktest.cpp`: simulation, not the game

This copies the four lock functions' bytes (the same bytes `Install()` requires) into executable memory. Their two KERNEL32 imports are pointed at this process's. It then runs:
- the same lock word as the modelled game lock, single-threaded: free, 5 readers, `0x0FFFFFFE` and `0x0FFFFFFF` readers, written by this thread, nested twice;
- uncontended and nested (read under own write);
- 12 simultaneous readers, mixing the game's and ours;
- a game writer holding the lock: the give-up timing, a one-try quick answer, the lock word untouched, and recovery;
- a writer that lets go mid-wait;
- a game writer waiting for our read;
- the policy:
  - quick give-ups while the same writer holds the lock;
  - a new hold waited for again;
  - a different writer waited for in full;
  - a pause ending the reuse;
  - the 2 s cap;
  - other groups in between not resetting it;
- reader races with no wait allowed: 300,000 tries, compared with 1.0's algorithm;
- the impossible count;
- 3 s of stress: 3 game writers (with nested reads through ours), 3 game readers and 4 bounded readers with 0–3 ms limits, checking mutual exclusion and untorn data.

```
xmake build locktest
xmake run locktest
```

It shows that the bounded reader and the game's lock code agree. It does **not** show anything about how the game uses these locks, or that the deadlock is fixed.

### In-game test plan

Use 1.6.1170 with SKSE and Address Library. Keep `CombatGroupDeadlockFix.log` from each session.

1. **Install check.** Load a save. The log shows `installed at SkyrimSE+803DF0` and no errors. Repeat with the plugin removed, and with it on a different runtime: SKSE's own log should show it isn't loaded there.
2. **Large battles.** Run 20 or more NPCs against each other: Whiterun or Solitude civil war sieges, a giant camp with followers, or summoned armies. Run 10 or more battles.
3. **Followers.** Use 3 or more followers (if a follower framework allows it), entering and leaving combat repeatedly.
4. **Combat start and end.** Aggro and flee repeatedly; yield, use calm effects, and fight in stealth (detection changes).
5. **Target switching.** Use a mixed-faction fight (Forsworn, Silver Hand and guards) where targets keep changing; use frenzy effects.
6. **Death in combat.** Kill group members mid-fight, kill the last target, and kill an actor while it is joining a fight.
7. **Cell transitions.** Leave and enter interiors mid-fight; fast travel during combat (where allowed); followers crossing load doors.
8. **Long sessions.** Play 3 or more hours with frequent combat. Watch for freezes, and for NPCs who stop fighting, stand idle while hostile, or won't re-engage.
9. **AI mods.** Repeat 2, 4 and 5 with the combat AI mods in use (for example, mods that change combat styles or detection), and with other SKSE freeze fixes installed.
10. **Read the log afterwards.**
    - Count the give-ups.
    - For each give-up, note whether a freeze would have been expected and whether combat carried on normally.
    - Check the slow-wait lines. Waits approaching 500 ms in normal play would argue for raising the limit.

Please report any give-up followed by broken combat behaviour, any freeze with the plugin installed, or slow waits near the limit, with the log.

## Build

```
git submodule update --init --recursive
xmake f -P . -y -p windows -a x64 -m release --skyrim_ae=y --skyrim_se=n --skyrim_vr=n
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```

`package.ps1` does the following:
- configures release, builds the plugin and `locktest`, and runs `locktest`;
- refuses a DLL older than the sources, or one whose file version doesn't match `xmake.lua`;
- writes `dist/CombatGroupDeadlockFix-<version>.zip`.
