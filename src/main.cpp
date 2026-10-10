// Combat Group Deadlock Fix (Skyrim AE 1.6.1170, SKSE).
//
// A vanilla engine deadlock, traced in Kev's game on 2026-10-09 (a debugger
// snapshot of the frozen process, then the code read from the running game):
//
//   Thread A starts combat (Actor::StartCombat, ID 38561). It takes the game's
//   global combat lock and a combat group's lock (CombatGroup + 0x160) for
//   WRITING, then (IDs 33228 -> 44704 -> 37759 -> 39515) wants to add an entry
//   to another actor's knowledge list, which needs that actor's AI-data lock
//   (MiddleHighProcessData + 0x238) for WRITING.
//
//   Thread B updates that same actor (ID 39578). It holds the actor's AI-data
//   lock for READING while it walks the knowledge list, and for each entry
//   (27293 -> 37534 -> 38571) asks "is this actor one of my combat group's
//   targets?" -- ID 44706, which wants the combat group's lock for READING.
//
//   Each waits for the lock the other holds; the game's lock never gives up,
//   and the main thread waits on both: the game freezes.
//
// The fix: ID 44706 is replaced by the same check, done the same way (the
// actor's handle, the group's lock for reading, a scan of the group's target
// list, then the lock released), except that it doesn't wait forever for the
// group's lock. If a writer holds it for more than half a second -- in normal
// play writers hold it for far less -- it gives up and answers "not a target"
// for that one check. Thread B can then finish, let go of the actor's lock,
// and thread A carries on. After one give-up, the same thread gives up at
// once on the same lock for a moment (thread B may ask once per knowledge
// entry), so the hitch stays short. Nothing else in the game is changed.

#include "pch.h"

#include <atomic>
#include <cstring>

#include <Windows.h>

namespace
{
    constexpr std::string_view kName = "CombatGroupDeadlockFix";
    constexpr std::uint64_t kGiveUpAfterMs = 500;
    constexpr std::uint64_t kFastFailForMs = 2000;

    // The game's BSReadWriteLock: the writing thread's id, then the count
    // (0x80000000 set while written; readers counted in the low bits).
    struct GameLock
    {
        volatile LONG writerThread;
        volatile LONG count;
    };
    constexpr LONG kWriterBit = static_cast<LONG>(0x80000000u);

    using GetHandleFn = std::uint32_t* (*)(RE::TESObjectREFR*, std::uint32_t*);
    GetHandleFn g_getHandle = nullptr;  // ID 37476, as 44706 calls it

    std::atomic<std::uint32_t> g_giveUps{ 0 };
    thread_local const GameLock* t_failedLock = nullptr;
    thread_local std::uint64_t t_failedAt = 0;

    // The game's LockForRead (ID 68233), copied, with a time limit.
    bool LockForReadWithin(GameLock* a_lock, std::uint64_t a_limitMs)
    {
        if (a_lock->writerThread == static_cast<LONG>(GetCurrentThreadId())) {
            InterlockedIncrement(&a_lock->count);  // this thread writes it: nested read
            return true;
        }
        const auto start = GetTickCount64();
        std::uint32_t spins = 0;
        for (;;) {
            const LONG expected = a_lock->count & 0x0FFFFFFF;
            if (InterlockedCompareExchange(&a_lock->count, expected + 1, expected) == expected) {
                return true;
            }
            if (GetTickCount64() - start >= a_limitMs) {
                return false;
            }
            Sleep(spins < 10000 ? 0 : 1);  // as the game does
            ++spins;
        }
    }

    // ID 44706, (CombatGroup*, actor) -> is the actor in the group's target
    // list. Same reads as the game's: the lock at +0x160, the targets'
    // data at +0x08 and count at +0x18, 0xA8 bytes each, handle first.
    bool IsGroupTarget(std::uint8_t* a_group, RE::TESObjectREFR* a_actor)
    {
        auto* lock = reinterpret_cast<GameLock*>(a_group + 0x160);
        const auto now = GetTickCount64();
        const bool recentlyFailedHere = t_failedLock == lock && now - t_failedAt < kFastFailForMs;
        if (!LockForReadWithin(lock, recentlyFailedHere ? 0 : kGiveUpAfterMs)) {
            if (!recentlyFailedHere) {
                t_failedLock = lock;
                t_failedAt = now;
                const auto count = ++g_giveUps;
                if (count <= 50 || count % 100 == 0) {
                    SKSE::log::warn("gave up waiting for a combat group's lock (held for writing by thread {} for {} ms); "
                                    "answered 'not a target' so the deadlock can clear (#{})",
                                    static_cast<std::uint32_t>(lock->writerThread), kGiveUpAfterMs, count);
                }
            }
            return false;
        }
        std::uint32_t handle = 0;
        const std::uint32_t wanted = *g_getHandle(a_actor, &handle);
        bool found = false;
        const auto targets = *reinterpret_cast<std::uint8_t**>(a_group + 0x08);
        const auto total = *reinterpret_cast<std::uint32_t*>(a_group + 0x18);
        for (std::uint32_t i = 0; i < total; ++i) {
            if (*reinterpret_cast<const std::uint32_t*>(targets + static_cast<std::size_t>(i) * 0xA8) == wanted) {
                found = true;
                break;
            }
        }
        InterlockedDecrement(&lock->count);  // UnlockForRead (ID 68239): lock dec [lock+4]
        return found;
    }

    bool Matches(std::uintptr_t a_address, std::initializer_list<std::uint8_t> a_bytes)
    {
        return std::memcmp(reinterpret_cast<const void*>(a_address), a_bytes.begin(), a_bytes.size()) == 0;
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
        const auto getHandle = REL::Relocation<std::uintptr_t>{ REL::ID(37476) }.address();
        // The game's 44706, checked where it matters (read from the running
        // 1.6.1170 game, 2026-10-09): its start, the lock at +0x160, the
        // calls to LockForRead and the handle function, the count at +0x18
        // and the 0xA8 stride.
        const bool ok =
            Matches(function, { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x30 }) &&
            Matches(function + 0x1F, { 0x48, 0x8D, 0x99, 0x60, 0x01, 0x00, 0x00 }) &&
            CallsTo(function + 0x2E, lockForRead) &&
            CallsTo(function + 0x3C, getHandle) &&
            Matches(function + 0x47, { 0x39, 0x56, 0x18 }) &&
            Matches(function + 0x62, { 0x48, 0x69, 0xC8, 0xA8, 0x00, 0x00, 0x00 });
        if (!ok) {
            SKSE::log::warn("not installed: the game's combat group check (ID 44706) isn't as expected; another mod or game version?");
            return;
        }
        g_getHandle = reinterpret_cast<GetHandleFn>(getHandle);
        SKSE::AllocTrampoline(14);
        SKSE::GetTrampoline().write_branch<5>(function, &IsGroupTarget);
        SKSE::log::info("installed at SkyrimSE+{:X} (ID 44706: the combat group's lock is waited for {} ms at most)",
                        function - REL::Module::get().base(), kGiveUpAfterMs);
    }
}

namespace
{
    constexpr SKSE::PluginVersionData MakePluginVersionData()
    {
        SKSE::PluginVersionData version{};
        version.PluginVersion({ 1, 0, 0, 0 });
        version.PluginName(kName);
        version.AuthorName("KezC7");
        version.UsesAddressLibrary();
        version.UsesNoStructs();
        return version;
    }
}

SKSE_PLUGIN_VERSION = MakePluginVersionData();

SKSE_EXPORT bool SKSEPlugin_Query(SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = kName.data();
    a_info->version = 1;
    return true;
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    REL::Module::reset();
    SKSE::Init(a_skse);
    SKSE::log::info("{} 1.0.0 loading", kName);
    Install();
    return true;
}
