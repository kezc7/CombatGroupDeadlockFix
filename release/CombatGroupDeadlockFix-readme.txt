Combat Group Deadlock Fix 1.0.0
Skyrim AE 1.6.1170 - needs SKSE64 and Address Library for SKSE Plugins

Fixes a freeze in the vanilla engine (a deadlock) that can happen as a fight
breaks out.

THE FREEZE
When combat starts, the game locks that fight's "combat group" (the list of
who is fighting whom) and then waits to update another NPC's knowledge of
the fight. At the same moment that NPC's own AI update holds its knowledge
list and asks the same combat group "is this person one of our targets?".
Each waits for the lock the other holds, the game's locks never give up, and
the game freezes.

THE FIX
That one "is this one of our targets?" check is replaced by the same check,
done the same way, except that it gives up waiting for the combat group's
lock after half a second and answers "not a target" for that one check. In
normal play the lock is held for a tiny fraction of that, so nothing
changes; only in the deadlock does it give up, which lets both sides carry
on. The worst case is a brief hitch instead of a freeze.

SAFETY
Every byte it relies on is checked first. If anything differs (another game
version, or another mod changed the same function), the plugin leaves the
game alone and says so in its log. Nothing is written to your save, so it
can be added or removed at any time.

LOG
Documents\My Games\Skyrim Special Edition\SKSE\CombatGroupDeadlockFix.log
- "installed at ..." line: the check was replaced.
- "gave up waiting for a combat group's lock ..." lines: each is a freeze
  the fix stopped.

INSTALL
Install with your mod manager, or copy SKSE\Plugins\CombatGroupDeadlockFix.dll into
Skyrim's Data\SKSE\Plugins folder. Remove it the same way.

Not for Skyrim SE 1.5.97 or VR. Works alongside other freeze and crash fixes,
including Skyrim Freeze Fix NG and Interrupt Package Deadlock Fix (which fix
different freezes).

Source (MIT): see the mod page.
