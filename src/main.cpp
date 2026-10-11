// Combat Group Deadlock Fix (Skyrim AE 1.6.1170, SKSE).
//
// A vanilla engine deadlock, traced in the author's game on 2026-10-09 (a debugger
// snapshot of the frozen process, then the code read from the running game):
//
//   Thread A starts combat (Actor::StartCombat, ID 38561). It takes the game's
//   global combat lock and a combat group's lock (CombatGroup + 0x160) for
//   WRITING, then (IDs 33228 -> 44704 -> 37759 -> 39515) wants to add an entry
//   to another actor's knowledge list, which needs that actor's AI-data lock
//   (MiddleHighProcessData + 0x238) for WRITING.
//
//   Thread B updates that same actor (ID 39578). It holds the actor's AI-data
//   lock for READING while it walks the knowledge list looking for a hostile
//   actor (27293), and for each entry asks whether it is hostile (37534 ->
//   38571), part of which is "is this actor one of that combat group's
//   targets?" -- ID 44706, which wants the combat group's lock for READING.
//
//   Each waits for the lock the other holds; the game's lock never gives up,
//   and the main thread waits on both: the game freezes.
//
// The fix: ID 44706 is replaced by the same check, done the same way (the
// group's lock for reading, the actor's handle, a scan of the group's target
// list, then the lock released), except that it doesn't wait forever for the
// group's lock. If a writer holds it for more than half a second -- in normal
// play writers hold it for far less -- it gives up and answers "not a target"
// for that one check. Thread B can then finish, let go of the actor's lock,
// and thread A carries on. While the same writer still holds the lock, the
// same thread then gives up at once (thread B asks once per knowledge entry),
// so the hitch stays short. Nothing else in the game is changed. GameLock.h
// has the lock; docs/TECHNICAL.md has the evidence.

#include "pch.h"

#include <atomic>
#include <cstring>

#include "GameFingerprints.h"
#include "GameLock.h"

namespace
{
    constexpr std::string_view kName = "CombatGroupDeadlockFix";
    constexpr REL::Version kVersion{ 1, 0, 1, 0 };

    // Half a second: far beyond any write hold in normal play (a hold that
    // long is already a visible hitch), short enough that the deadlock costs
    // a hitch instead of a freeze. After a give-up, the same thread gives up
    // at once while the same writer holds the lock and the thread keeps
    // asking (no pause of 50 ms: about three ticks of GetTickCount64, far
    // longer than the gap between two asks of one knowledge walk), for at
    // most 2 s; then it waits in full again. See LockGroupForRead.
    constexpr cgdf::GroupLockPolicy kPolicy{ .giveUpAfterMs = 500, .quickAnswerForMs = 2000, .quickGapMs = 50 };
    // Waits this long that still got the lock are logged, to show how close
    // normal play comes to the give-up time.
    constexpr std::uint64_t kSlowWaitMs = 100;

    using GetHandleFn = std::uint32_t* (*)(RE::TESObjectREFR*, std::uint32_t*);
    GetHandleFn g_getHandle = nullptr;  // ID 37476, as 44706 calls it

    std::atomic<std::uint32_t> g_giveUps{ 0 };
    std::atomic<std::uint32_t> g_quickGiveUps{ 0 };
    std::atomic<std::uint32_t> g_slowWaits{ 0 };

    // The first few of each, then every 100th, so a bad case can't flood the log.
    bool ShouldLog(std::uint32_t a_count)
    {
        return a_count <= 20 || a_count % 100 == 0;
    }

    // ID 44706, (CombatGroup*, actor) -> is the actor in the group's target
    // list. Same reads as the game's: the lock at +0x160, the targets' data at
    // +0x08 and count at +0x18, 0xA8 bytes each, handle first. Returns 0 or 1
    // in all of eax, as the game's does (movzx eax, dil).
    std::uint32_t IsGroupTarget(std::uint8_t* a_group, RE::TESObjectREFR* a_actor)
    {
        auto* lock = reinterpret_cast<cgdf::GameLock*>(a_group + 0x160);
        const auto lockResult = cgdf::LockGroupForRead(lock, kPolicy);
        if (lockResult.outcome == cgdf::GroupLock::kQuickGaveUp) {
            ++g_quickGiveUps;
            return 0;
        }
        if (lockResult.outcome == cgdf::GroupLock::kGaveUp) {
            const auto count = ++g_giveUps;
            if (ShouldLog(count)) {
                SKSE::log::warn("gave up after {} ms waiting for a combat group's lock (written by thread {}); "
                                "answered 'not a target' so the deadlock can clear (give-up #{}, {} quick give-ups so far)",
                                lockResult.waitedMs, static_cast<std::uint32_t>(lockResult.writer), count, g_quickGiveUps.load());
            }
            return 0;
        }

        std::uint32_t handle = 0;
        const std::uint32_t wanted = *g_getHandle(a_actor, &handle);
        bool found = false;
        const auto total = *reinterpret_cast<const std::uint32_t*>(a_group + 0x18);
        if (total != 0) {
            const auto targets = *reinterpret_cast<const std::uint8_t* const*>(a_group + 0x08);
            for (std::uint32_t i = 0; i < total; ++i) {
                if (*reinterpret_cast<const std::uint32_t*>(targets + static_cast<std::size_t>(i) * 0xA8) == wanted) {
                    found = true;
                    break;
                }
            }
        }
        cgdf::UnlockForRead(lock);

        if (lockResult.waitedMs >= kSlowWaitMs) {
            const auto count = ++g_slowWaits;
            if (ShouldLog(count)) {
                SKSE::log::info("waited {} ms for a combat group's lock, then got it (slow wait #{})", lockResult.waitedMs, count);
            }
        }
        return found ? 1 : 0;
    }

    bool CallsTo(std::uintptr_t a_site, std::uintptr_t a_target)
    {
        if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8) {
            return false;
        }
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(a_site + 1), sizeof(rel));
        return a_site + 5 + rel == a_target;
    }

    void Install()
    {
        const auto function = REL::Relocation<std::uintptr_t>{ REL::ID(44706) }.address();
        const auto lockForRead = REL::Relocation<std::uintptr_t>{ REL::ID(68233) }.address();
        const auto lockForWrite = REL::Relocation<std::uintptr_t>{ REL::ID(68234) }.address();
        const auto unlockForRead = REL::Relocation<std::uintptr_t>{ REL::ID(68239) }.address();
        const auto unlockForWrite = REL::Relocation<std::uintptr_t>{ REL::ID(68240) }.address();
        const auto getHandle = REL::Relocation<std::uintptr_t>{ REL::ID(37476) }.address();

        // All of 44706, and all four lock functions whose protocol the
        // replacement has to share, must be exactly as in 1.6.1170: their
        // fingerprints (length and hash) must match.
        struct Check
        {
            const char* what;
            bool        ok;
        };
        const Check checks[] = {
            { "combat group check (ID 44706)", cgdf::code::Matches(function, cgdf::code::kIsGroupTarget) },
            { "its call to LockForRead", CallsTo(function + cgdf::code::kIsGroupTargetCallLockForRead, lockForRead) },
            { "its call to the handle function (ID 37476)", CallsTo(function + cgdf::code::kIsGroupTargetCallGetHandle, getHandle) },
            { "its call to UnlockForRead", CallsTo(function + cgdf::code::kIsGroupTargetCallUnlockForRead, unlockForRead) },
            { "LockForRead (ID 68233)", cgdf::code::Matches(lockForRead, cgdf::code::kLockForRead) },
            { "LockForWrite (ID 68234)", cgdf::code::Matches(lockForWrite, cgdf::code::kLockForWrite) },
            { "UnlockForRead (ID 68239)", cgdf::code::Matches(unlockForRead, cgdf::code::kUnlockForRead) },
            { "UnlockForWrite (ID 68240)", cgdf::code::Matches(unlockForWrite, cgdf::code::kUnlockForWrite) },
        };
        bool ok = true;
        for (const auto& check : checks) {
            if (!check.ok) {
                SKSE::log::error("the game's {} isn't as expected (another mod changed it?)", check.what);
                ok = false;
            }
        }
        if (!ok) {
            SKSE::log::error("not installed: the game has been left unchanged");
            return;
        }

        g_getHandle = reinterpret_cast<GetHandleFn>(getHandle);
        SKSE::AllocTrampoline(14);
        SKSE::GetTrampoline().write_branch<5>(function, &IsGroupTarget);
        SKSE::log::info("installed at SkyrimSE+{:X} (ID 44706): a combat group's lock is waited for {} ms at most",
                        function - REL::Module::get().base(), kPolicy.giveUpAfterMs);
    }
}

namespace
{
    constexpr SKSE::PluginVersionData MakePluginVersionData()
    {
        SKSE::PluginVersionData version{};
        version.PluginVersion(kVersion);
        version.PluginName(kName);
        version.AuthorName("KezC7");
        // Only the runtime this was verified on; SKSE won't load it on others.
        version.CompatibleVersions({ SKSE::RUNTIME_SSE_1_6_1170 });
        version.UsesNoStructs();
        return version;
    }
}

SKSE_PLUGIN_VERSION = MakePluginVersionData();

SKSE_EXPORT bool SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = kName.data();
    a_info->version = kVersion.pack();
    return a_skse->RuntimeVersion() == SKSE::RUNTIME_SSE_1_6_1170;  // only older SKSEs (SE 1.5.97) ask this
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    // Checked before SKSE::Init, which would load the Address Library for
    // whatever runtime this is.
    if (a_skse->RuntimeVersion() != SKSE::RUNTIME_SSE_1_6_1170) {
        return false;
    }
    REL::Module::reset();
    SKSE::Init(a_skse);  // logs the name and version
    Install();
    return true;
}
