Combat Group Deadlock Fix 1.0.0
Skyrim AE 1.6.1170 only - needs SKSE64 and Address Library for SKSE Plugins

Fixes a freeze in the vanilla engine (a deadlock) that can happen as a fight
breaks out.

THE FREEZE
When combat starts, the game locks that fight's "combat group" (the list of
who is fighting whom). It then waits to update another NPC's knowledge of the
fight.

At the same moment, that NPC's own AI update holds its knowledge list while it
looks for enemies. For each one it asks the same combat group "is this person
one of your targets?".

Each side waits for the lock the other holds. The game's locks never give up,
so the game freezes.

THE FIX
That one "is this one of your targets?" check is replaced by the same check,
done the same way, with one difference: if it can't get the combat group's
lock within half a second, it gives up and answers "not a target" for that
check.
- While the same stuck lock remains, that AI update gets the same answer at
  once, so the hitch stays short.
- In normal play the lock is held for a tiny fraction of a second, so nothing
  changes.
- In the freeze, both sides can carry on. The worst case is a hitch of about
  half a second instead of a hard freeze.

What "not a target" can do: in that moment, one NPC may not count someone as
an enemy, so it may not react to them. The next AI update asks again. This
only happens after the lock has been held for half a second, which in the
unmodified game would have been a freeze or a long stall.

SAFETY
- It only loads on Skyrim 1.6.1170.
- Before changing anything, it fingerprints all of the game code it relies
  on. If anything differs (another game version, or another mod changed the
  same code), it leaves the game alone and says why in its log.
- Nothing is written to your save, so it can be added or removed at any time.

INSTALL
Install with your mod manager. Or copy SKSE\Plugins\CombatGroupDeadlockFix.dll
into Skyrim's Data\SKSE\Plugins folder. Remove it the same way.

LOG
Documents\My Games\Skyrim Special Edition\SKSE\CombatGroupDeadlockFix.log
- "installed at SkyrimSE+803DF0 ...": working.
- "not installed ...", with the lines above it: the game code isn't as
  expected, and nothing was changed.
- "gave up after ... ms waiting for a combat group's lock ...": a combat group
  was locked for half a second or more. Most likely the freeze this fixes.
- "waited ... ms for a combat group's lock, then got it": a slow moment that
  the fix didn't need to step in for.
No log file at all: SKSE didn't load the plugin. Check skse64.log for the
reason; on any version other than 1.6.1170, that's expected.

TROUBLESHOOTING
- Still freezing in combat? Keep CombatGroupDeadlockFix.log and a crash or
  freeze dump, and report them. Not every combat freeze is this one.
- NPCs ignoring enemies right after a "gave up" line? Report it with the log.
  That is the one known side effect, and it should not last.

COMPATIBILITY
Works alongside other freeze and crash fixes, including Skyrim Freeze Fix NG
and Interrupt Package Deadlock Fix, which fix different freezes. If another
mod changes the same game code, this one refuses to install and logs it.

Not for Skyrim SE 1.5.97, other AE versions, or VR.

Source (MIT): see the mod page.
