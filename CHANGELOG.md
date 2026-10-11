# Changelog

## 1.1.0 (2026-10-11)

An audit against the 1.6.1170 machine code (see [docs/TECHNICAL.md](docs/TECHNICAL.md)).

**Fixes**
- **Fast-fail no longer answers "not a target" when no writer holds the lock.** 1.0's fast-fail mode counted a lost race with another *reader* as a failure. For 2 s after any give-up, that thread could get false answers about the group while only readers were around: 14–48% of tries in the lock test. The read-lock now retries reader races at once, as the game's `LockForRead` does, and only waits on a writer.
- **Quick give-ups are limited to the hold that caused them.** 1.0 reused a give-up for 2 s, keyed only on the lock's address. That covered any later short write by any thread, and any new group at the same address, and those answers were never logged. Now a quick give-up needs all of these:
  - the same writer still holds the lock;
  - the thread is still asking back to back, with no pause of 50 ms or more;
  - less than 2 s since the full wait.
  
  Taking the lock forgets the give-up.
- **Only loads on Skyrim AE 1.6.1170.** 1.0 declared Address Library version independence, so SKSE would load it on any AE runtime, including 1.7.x. It now declares 1.6.1170 as its only compatible runtime, and checks again before anything else.
- **Verifies everything it depends on before patching.** 1.0 checked about 30 of the 166 bytes of ID 44706. 1.1 checks:
  - all 166 bytes of 44706;
  - all of the game's four lock functions (LockForRead, LockForWrite, UnlockForRead, UnlockForWrite), because the replacement shares their protocol;
  - the targets of 44706's three calls.
  
  Each failed check is logged by name.
- **Returns 0 or 1 in all of `eax`,** as the game's function does. 1.0 returned a C++ `bool`, which only defines `al`. All callers that can be found test `al`, but some are virtual calls.

**Other changes**
- Waits of 100 ms or more that still get the lock are logged (rate-limited), to show how close normal play comes to the 500 ms limit.
- Logging is rate-limited to the first 20 of each kind, then every 100th, and never happens while the combat group's lock is held.
- The DLL carries a version resource (1.1.0); 1.0's read 0.0.0.
- Added `tests/locktest`, which runs the bounded read-lock against a model of the game's lock.
- Install checks compare length and FNV-1a 64 fingerprints of the game's code (`src/GameFingerprints.h`), not copies of it; no game code is in the repository or the DLL.
- The packaging script always builds release, runs the lock test, and refuses a stale DLL.

## 1.0.0 (2026-10-09)

First version. Replaces ID 44706 with a version that stops waiting for a combat group's lock after 500 ms.
