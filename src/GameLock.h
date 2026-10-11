#pragma once

// The game's BSReadWriteLock as Skyrim AE 1.6.1170 uses it (IDs 68233,
// 68234, 68239, 68240; fingerprinted in GameFingerprints.h), and a read-lock that
// stops waiting for a writer after a time limit. Kept free of CommonLib so the
// lock test can build it on its own.
//
// The lock, from the game's code:
//   +0 writerThread  id of the writing thread; set after the writer gets the
//                    lock, cleared before it lets go.
//   +4 count         0x80000000 while written, plus 1 per hold: the writer's
//                    first hold makes it 0x80000001, and the writer's nested
//                    reads and writes add to it. Otherwise the number of readers.
// Readers only get in while bits 28..31 of the count are clear (they swap
// count & 0x0FFFFFFF for one more), so never while it's written. A writer only
// gets in when the count is 0: no readers. Nobody marks a writer as waiting,
// so readers can keep a writer out; the game is built that way. Every change
// to the count is a lock-prefixed instruction (full barrier on x64).

#include <cstdint>

#include <Windows.h>

namespace cgdf
{
    struct GameLock
    {
        volatile LONG writerThread;  // 0
        volatile LONG count;         // 4
    };
    static_assert(sizeof(GameLock) == 0x8);

    inline constexpr LONG kCountMask = 0x0FFFFFFF;  // the mask LockForRead applies to the count

    // LockForRead (ID 68233), step for step, except that waiting for a writer
    // stops after a_limitMs (0: don't wait at all) and it then returns false
    // without the lock. a_waitedMs is how long it waited for a writer.
    //
    // A failed swap because another reader moved the count is retried at
    // once, as the game does; only a writer is waited for. The game would also
    // retry forever on a count with bits 28..30 set and bit 31 clear, a state
    // its own code never makes (it would take 2^28 holds); this treats that as
    // a writer, so it ends at the time limit instead.
    inline bool LockForReadWithin(GameLock* a_lock, std::uint64_t a_limitMs, std::uint64_t& a_waitedMs)
    {
        a_waitedMs = 0;
        if (a_lock->writerThread == static_cast<LONG>(GetCurrentThreadId())) {
            InterlockedIncrement(&a_lock->count);  // this thread writes it: a nested read
            return true;
        }
        LONG seen = a_lock->count;
        std::uint64_t start = 0;
        bool waiting = false;
        std::uint32_t sleeps = 0;
        for (;;) {
            const LONG expected = seen & kCountMask;
            seen = InterlockedCompareExchange(&a_lock->count, expected + 1, expected);
            if (seen == expected) {
                if (waiting) {
                    a_waitedMs = GetTickCount64() - start;
                }
                return true;
            }
            if ((seen & ~kCountMask) == 0) {
                continue;  // another reader got in first
            }
            const auto now = GetTickCount64();
            if (!waiting) {
                waiting = true;
                start = now;
            }
            if (now - start >= a_limitMs) {
                a_waitedMs = now - start;
                return false;
            }
            if (sleeps < 10000) {
                ++sleeps;
                Sleep(0);
            } else {
                Sleep(1);
            }
            seen = a_lock->count;
        }
    }

    // UnlockForRead (ID 68239): lock dec count, in every state.
    inline void UnlockForRead(GameLock* a_lock)
    {
        InterlockedDecrement(&a_lock->count);
    }

    struct GroupLockPolicy
    {
        std::uint64_t giveUpAfterMs;     // longest wait for one writer
        std::uint64_t quickAnswerForMs;  // longest a give-up is reused for (see below)
        std::uint64_t quickGapMs;        // a pause between asks this long ends the reuse
    };

    enum class GroupLock
    {
        kAcquired,     // got it (a_waitedMs says how long that took)
        kGaveUp,       // waited giveUpAfterMs for a writer, didn't get it
        kQuickGaveUp,  // the writer this thread already gave up on still has it
    };

    struct GroupLockResult
    {
        GroupLock     outcome;
        std::uint64_t waitedMs;
        LONG          writer;  // the writing thread, when not acquired
    };

    namespace detail
    {
        struct LastGiveUp
        {
            const GameLock* lock;
            LONG            writer;
            std::uint64_t   gaveUpAt;  // the full wait that ended in a give-up
            std::uint64_t   askedAt;   // the latest give-up, full or quick
        };
        inline thread_local LastGiveUp t_lastGiveUp{};
    }

    // A read-lock on a combat group's lock that can't wait forever.
    //
    // A thread that has given up on a writer is usually still in the middle of
    // the walk that asks once per knowledge entry, still holding the lock the
    // writer is waiting for, so the writer can't have moved on. Waiting the
    // full time again for each entry would make the hitch last entries x the
    // limit. So once it has given up, the same thread gives up at once on the
    // same lock, but only while all of these hold:
    //   - the lock is still written, by the same thread;
    //   - this thread keeps asking: less than quickGapMs since its last
    //     give-up. A walk asks back to back; a pause means it has ended, and a
    //     later hold by the same writer thread (a thread id doesn't tell holds
    //     apart) is waited for normally;
    //   - less than quickAnswerForMs since the full wait, so even an endless
    //     run of asks waits in full again now and then.
    // Even a quick give-up tries the lock once, so a lock that has come free
    // is taken. Getting that lock forgets the give-up; asks about other
    // locks (the walk asks about many actors' groups) leave it alone. One
    // give-up is remembered per thread: the deadlock involves one group.
    inline GroupLockResult LockGroupForRead(GameLock* a_lock, const GroupLockPolicy& a_policy)
    {
        auto& last = detail::t_lastGiveUp;
        bool quick = false;
        if (last.lock == a_lock) {
            const LONG count = a_lock->count;
            const LONG writer = a_lock->writerThread;
            const auto now = GetTickCount64();
            quick = count < 0 && writer == last.writer &&
                    now - last.askedAt < a_policy.quickGapMs && now - last.gaveUpAt < a_policy.quickAnswerForMs;
            if (!quick) {
                last = {};
            }
        }
        std::uint64_t waited = 0;
        if (LockForReadWithin(a_lock, quick ? 0 : a_policy.giveUpAfterMs, waited)) {
            if (last.lock == a_lock) {
                last = {};
            }
            return { GroupLock::kAcquired, waited, 0 };
        }
        const LONG writer = a_lock->writerThread;
        const auto now = GetTickCount64();
        if (quick) {
            last.askedAt = now;
            return { GroupLock::kQuickGaveUp, 0, writer };
        }
        // While a writer is getting or letting go of the lock its id can read
        // as 0; don't take that as "the same writer" next time.
        if (writer != 0) {
            last = { a_lock, writer, now, now };
        }
        return { GroupLock::kGaveUp, waited, writer };
    }
}
