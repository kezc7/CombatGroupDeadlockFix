// Tests the bounded read-lock (src/GameLock.h) against a model of the game's
// lock.
//
// The writers and plain readers below run a C++ model of Skyrim AE 1.6.1170's
// BSReadWriteLock, written from its behaviour as described in
// src/GameFingerprints.h and docs/TECHNICAL.md (no game code is used or
// shipped here). Install() only patches a game whose lock functions match the
// fingerprints of the code that behaviour was read from.
//
// What this shows: the bounded reader and the modelled lock agree on the lock
// word in every state tested, under real threads. What it doesn't: how the
// game uses its combat group locks, or anything about the deadlock itself.
// That needs the game (docs/TECHNICAL.md, "In-game test plan").

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "GameLock.h"

using cgdf::GameLock;

namespace
{
    using LockFn = void (*)(GameLock*);

    struct GameLockCode
    {
        LockFn lockForRead;
        LockFn lockForWrite;
        LockFn unlockForRead;
        LockFn unlockForWrite;
    };

    // The modelled game lock (see the comment at the top of this file).
    void ModelSleep(std::uint32_t& a_sleeps)
    {
        if (a_sleeps < 10000) {
            ++a_sleeps;
            Sleep(0);
        } else {
            Sleep(1);
        }
    }

    void ModelLockForRead(GameLock* a_lock)
    {
        if (a_lock->writerThread == static_cast<LONG>(GetCurrentThreadId())) {
            InterlockedIncrement(&a_lock->count);
            return;
        }
        std::uint32_t sleeps = 0;
        LONG seen = a_lock->count;
        for (;;) {
            const LONG expected = seen & cgdf::kCountMask;
            seen = InterlockedCompareExchange(&a_lock->count, expected + 1, expected);
            if (seen == expected) {
                return;
            }
            if (seen >= 0) {
                continue;  // no writer bit: retry at once
            }
            ModelSleep(sleeps);
            seen = a_lock->count;
        }
    }

    void ModelLockForWrite(GameLock* a_lock)
    {
        const LONG self = static_cast<LONG>(GetCurrentThreadId());
        if (a_lock->writerThread == self) {
            InterlockedIncrement(&a_lock->count);
            return;
        }
        std::uint32_t sleeps = 0;
        while (InterlockedCompareExchange(&a_lock->count, static_cast<LONG>(0x80000001u), 0) != 0) {
            ModelSleep(sleeps);
        }
        a_lock->writerThread = self;
        MemoryBarrier();
    }

    void ModelUnlockForRead(GameLock* a_lock)
    {
        InterlockedDecrement(&a_lock->count);
    }

    void ModelUnlockForWrite(GameLock* a_lock)
    {
        if (a_lock->count == static_cast<LONG>(0x80000001u)) {
            a_lock->writerThread = 0;
            MemoryBarrier();
            InterlockedExchange(&a_lock->count, 0);
        } else {
            InterlockedDecrement(&a_lock->count);
        }
    }

    const GameLockCode g_game{ ModelLockForRead, ModelLockForWrite, ModelUnlockForRead, ModelUnlockForWrite };

    int g_failures = 0;
    int g_checks = 0;

    void Check(bool a_ok, const char* a_what)
    {
        ++g_checks;
        if (!a_ok) {
            ++g_failures;
            std::printf("    FAIL: %s\n", a_what);
        }
    }

    void Section(const char* a_name)
    {
        std::printf("[%s]\n", a_name);
    }

    LONG Tid()
    {
        return static_cast<LONG>(GetCurrentThreadId());
    }

    bool Is(const GameLock& a_lock, LONG a_writer, std::uint32_t a_count)
    {
        return a_lock.writerThread == a_writer && static_cast<std::uint32_t>(a_lock.count) == a_count;
    }

    double MsSince(std::chrono::steady_clock::time_point a_start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
    }

    bool OurRead(GameLock& a_lock, std::uint64_t a_limitMs, std::uint64_t* a_waited = nullptr)
    {
        std::uint64_t waited = 0;
        const bool ok = cgdf::LockForReadWithin(&a_lock, a_limitMs, waited);
        if (a_waited) {
            *a_waited = waited;
        }
        return ok;
    }

    // Holds a lock on another thread with the modelled game lock until told to let go.
    class Holder
    {
    public:
        Holder(GameLock& a_lock, bool a_write) :
            _lock(a_lock), _write(a_write), _thread([this] { Run(); })
        {
            while (!_holding.load()) {
                std::this_thread::yield();
            }
        }
        ~Holder()
        {
            Release();
        }
        void Release()
        {
            if (_thread.joinable()) {
                _release = true;
                _thread.join();
            }
        }
        LONG tid() const { return _tid.load(); }

    private:
        void Run()
        {
            _tid = Tid();
            (_write ? g_game.lockForWrite : g_game.lockForRead)(&_lock);
            _holding = true;
            while (!_release.load()) {
                Sleep(1);
            }
            (_write ? g_game.unlockForWrite : g_game.unlockForRead)(&_lock);
        }

        GameLock&         _lock;
        bool              _write;
        std::atomic<bool> _holding{ false };
        std::atomic<bool> _release{ false };
        std::atomic<LONG> _tid{ 0 };
        std::thread       _thread;
    };

    // The 1.0 read-lock, kept only to show the defect fixed in 1.1: it treated
    // a lost race with another reader like a writer, so with no wait allowed
    // (its fast-fail mode) it could answer "not a target" with no writer at all.
    bool LegacyLockForReadWithin(GameLock* a_lock, std::uint64_t a_limitMs)
    {
        if (a_lock->writerThread == Tid()) {
            InterlockedIncrement(&a_lock->count);
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
            Sleep(spins < 10000 ? 0 : 1);
            ++spins;
        }
    }

    void TestSingleThreadEquivalence()
    {
        Section("same lock word as the modelled game lock, one thread");
        const LONG me = Tid();
        struct Case
        {
            LONG          writer;
            std::uint32_t count;
            const char*   name;
        };
        const Case cases[] = {
            { 0, 0, "free" },
            { 0, 5, "5 readers" },
            { 0, 0x0FFFFFFE, "0x0FFFFFFE readers" },
            { 0, 0x0FFFFFFF, "0x0FFFFFFF readers (the game takes it to 0x10000000 too)" },
            { me, 0x80000001, "written by this thread" },
            { me, 0x80000003, "written by this thread, nested twice" },
        };
        for (const auto& c : cases) {
            GameLock game{ c.writer, static_cast<LONG>(c.count) };
            GameLock ours = game;
            g_game.lockForRead(&game);
            std::uint64_t waited = 99;
            const bool ok = OurRead(ours, 0, &waited);
            char what[160];
            std::snprintf(what, sizeof(what), "read-lock from %s: game -> {%ld, %08lX}, ours -> {%ld, %08lX}",
                          c.name, game.writerThread, game.count, ours.writerThread, ours.count);
            Check(ok && waited == 0 && std::memcmp(&game, &ours, sizeof(GameLock)) == 0, what);

            g_game.unlockForRead(&game);
            cgdf::UnlockForRead(&ours);
            std::snprintf(what, sizeof(what), "read-unlock back from %s", c.name);
            Check(std::memcmp(&game, &ours, sizeof(GameLock)) == 0 && Is(ours, c.writer, c.count), what);
        }
    }

    void TestUncontendedAndNested()
    {
        Section("uncontended and nested");
        GameLock lock{};
        Check(OurRead(lock, 500) && Is(lock, 0, 1), "uncontended: one reader");
        g_game.unlockForRead(&lock);
        Check(Is(lock, 0, 0), "game's UnlockForRead releases ours");
        g_game.lockForRead(&lock);
        cgdf::UnlockForRead(&lock);
        Check(Is(lock, 0, 0), "ours releases the game's read");

        g_game.lockForWrite(&lock);
        Check(Is(lock, Tid(), 0x80000001), "game write lock on this thread");
        Check(OurRead(lock, 0) && Is(lock, Tid(), 0x80000002), "nested read under own write, even with no wait allowed");
        Check(OurRead(lock, 0) && Is(lock, Tid(), 0x80000003), "twice nested");
        cgdf::UnlockForRead(&lock);
        cgdf::UnlockForRead(&lock);
        Check(Is(lock, Tid(), 0x80000001), "nested reads undone");
        g_game.unlockForWrite(&lock);
        Check(Is(lock, 0, 0), "game write unlock leaves it free");

        auto policyLock = GameLock{};
        g_game.lockForWrite(&policyLock);
        const auto r = cgdf::LockGroupForRead(&policyLock, { 100, 1000 });
        Check(r.outcome == cgdf::GroupLock::kAcquired && Is(policyLock, Tid(), 0x80000002), "policy: nested read under own write");
        cgdf::UnlockForRead(&policyLock);
        g_game.unlockForWrite(&policyLock);
    }

    void TestMultipleReaders()
    {
        Section("many readers at once");
        GameLock lock{};
        constexpr int kThreads = 12;
        std::atomic<int> in{ 0 };
        std::atomic<bool> go{ false };
        std::atomic<int> failed{ 0 };
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                const bool ok = (i % 2) ? OurRead(lock, 500) : (g_game.lockForRead(&lock), true);
                if (!ok) {
                    ++failed;
                }
                ++in;
                while (!go.load()) {
                    Sleep(1);
                }
                (i % 3 ? cgdf::UnlockForRead : g_game.unlockForRead)(&lock);
            });
        }
        while (in.load() < kThreads) {
            Sleep(1);
        }
        Check(failed == 0 && Is(lock, 0, kThreads), "12 readers (ours and the game's) hold it together");
        go = true;
        for (auto& t : threads) {
            t.join();
        }
        Check(Is(lock, 0, 0), "all released");
    }

    void TestActiveWriter()
    {
        Section("a writer holds it");
        GameLock lock{};
        {
            Holder writer(lock, true);
            const LONG w = writer.tid();
            Check(Is(lock, w, 0x80000001), "game writer holds it");

            const auto start = std::chrono::steady_clock::now();
            std::uint64_t waited = 0;
            const bool ok = OurRead(lock, 100, &waited);
            const auto ms = MsSince(start);
            std::printf("    gave up after %.1f ms (limit 100, counted %llu)\n", ms, waited);
            Check(!ok, "no read lock while written");
            Check(ms >= 80 && ms < 300, "gave up about when it should");
            Check(waited >= 100, "reports the wait");
            Check(Is(lock, w, 0x80000001), "lock word untouched by the give-up");

            const auto quick = std::chrono::steady_clock::now();
            Check(!OurRead(lock, 0) && MsSince(quick) < 20, "no wait allowed: one try, then false");
            Check(Is(lock, w, 0x80000001), "still untouched");
            writer.Release();
        }
        Check(Is(lock, 0, 0), "writer gone");
        Check(OurRead(lock, 0) && Is(lock, 0, 1), "recovers at once when the writer has gone");
        cgdf::UnlockForRead(&lock);
    }

    void TestWriterReleasesDuringWait()
    {
        Section("the writer lets go while we wait");
        GameLock lock{};
        std::atomic<bool> holding{ false };
        std::thread writer([&] {
            g_game.lockForWrite(&lock);
            holding = true;
            Sleep(150);
            g_game.unlockForWrite(&lock);
        });
        while (!holding.load()) {
            std::this_thread::yield();
        }
        std::uint64_t waited = 0;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = OurRead(lock, 1000, &waited);
        const auto ms = MsSince(start);
        writer.join();
        std::printf("    got it after %.1f ms (counted %llu)\n", ms, waited);
        Check(ok && Is(lock, 0, 1), "got the read lock once the writer let go");
        Check(ms >= 100 && ms < 600, "waited for the writer, not the limit");
        cgdf::UnlockForRead(&lock);
        Check(Is(lock, 0, 0), "released");
    }

    void TestWriterWaitsForUs()
    {
        Section("a writer waits for our read");
        GameLock lock{};
        Check(OurRead(lock, 0), "we read-lock it");
        std::atomic<bool> writing{ false };
        std::atomic<LONG> writerTid{ 0 };
        std::thread writer([&] {
            writerTid = Tid();
            g_game.lockForWrite(&lock);
            writing = true;
            Sleep(20);
            g_game.unlockForWrite(&lock);
        });
        Sleep(100);
        Check(!writing.load() && Is(lock, 0, 1), "game writer can't get in while we read");
        cgdf::UnlockForRead(&lock);
        const auto start = std::chrono::steady_clock::now();
        while (!writing.load() && MsSince(start) < 2000) {
            Sleep(1);
        }
        Check(writing.load(), "game writer gets in once we let go");
        writer.join();
        Check(Is(lock, 0, 0), "and leaves it free");
    }

    void TestPolicy()
    {
        Section("give-up policy");
        constexpr cgdf::GroupLockPolicy policy{ .giveUpAfterMs = 100, .quickAnswerForMs = 600, .quickGapMs = 50 };
        GameLock lock{};
        {
            Holder writer(lock, true);
            auto start = std::chrono::steady_clock::now();
            auto r = cgdf::LockGroupForRead(&lock, policy);
            Check(r.outcome == cgdf::GroupLock::kGaveUp && r.writer == writer.tid() && MsSince(start) >= 80, "first: waits, gives up, names the writer");

            start = std::chrono::steady_clock::now();
            for (int i = 0; i < 50; ++i) {
                r = cgdf::LockGroupForRead(&lock, policy);
                if (r.outcome != cgdf::GroupLock::kQuickGaveUp) {
                    break;
                }
            }
            const auto ms = MsSince(start);
            std::printf("    50 more asks while the same writer holds it: %.2f ms\n", ms);
            Check(r.outcome == cgdf::GroupLock::kQuickGaveUp && ms < 50, "same writer still holds it: gives up at once");
            Check(Is(lock, writer.tid(), 0x80000001), "lock word untouched");
            writer.Release();
        }
        auto r = cgdf::LockGroupForRead(&lock, policy);
        Check(r.outcome == cgdf::GroupLock::kAcquired && Is(lock, 0, 1), "writer gone: takes the lock at once");
        cgdf::UnlockForRead(&lock);

        // A brief write by the same thread after recovery must be waited for,
        // not answered from the old give-up.
        {
            Holder writer(lock, true);
            const auto start = std::chrono::steady_clock::now();
            r = cgdf::LockGroupForRead(&lock, policy);
            Check(r.outcome == cgdf::GroupLock::kGaveUp && MsSince(start) >= 80, "a new hold is waited for again");
        }
        r = cgdf::LockGroupForRead(&lock, policy);
        Check(r.outcome == cgdf::GroupLock::kAcquired, "and recovers");
        cgdf::UnlockForRead(&lock);

        // A different writer: not the hold we gave up on. Writer 2's thread
        // is started while writer 1's is alive, so their ids differ.
        {
            auto first = std::make_unique<Holder>(lock, true);
            std::atomic<bool> go{ false }, holding{ false }, release{ false };
            std::atomic<LONG> secondTid{ 0 };
            std::thread second([&] {
                secondTid = Tid();
                while (!go.load()) {
                    Sleep(1);
                }
                g_game.lockForWrite(&lock);
                holding = true;
                while (!release.load()) {
                    Sleep(1);
                }
                g_game.unlockForWrite(&lock);
            });
            r = cgdf::LockGroupForRead(&lock, policy);
            Check(r.outcome == cgdf::GroupLock::kGaveUp, "gave up on writer 1");
            go = true;
            first.reset();
            while (!holding.load()) {
                std::this_thread::yield();
            }
            const auto start = std::chrono::steady_clock::now();
            r = cgdf::LockGroupForRead(&lock, policy);
            Check(r.outcome == cgdf::GroupLock::kGaveUp && MsSince(start) >= 80 && r.writer == secondTid.load(),
                  "writer 2 is waited for in full");
            release = true;
            second.join();
        }

        // The same writer still holds it, but this thread paused (its walk is
        // over): waits in full again.
        {
            Holder writer(lock, true);
            r = cgdf::LockGroupForRead(&lock, policy);
            Check(r.outcome == cgdf::GroupLock::kGaveUp, "gave up");
            Sleep(120);
            const auto start = std::chrono::steady_clock::now();
            r = cgdf::LockGroupForRead(&lock, policy);
            Check(r.outcome == cgdf::GroupLock::kGaveUp && MsSince(start) >= 80, "after a pause: waits in full again");
        }

        // Asking on and on while the same writer holds it: quick, but a full
        // wait again once quickAnswerForMs has passed.
        {
            Holder writer(lock, true);
            r = cgdf::LockGroupForRead(&lock, policy);
            const auto start = std::chrono::steady_clock::now();
            int quick = 0;
            double fullAt = -1;
            while (MsSince(start) < 900) {
                const auto askStart = std::chrono::steady_clock::now();
                r = cgdf::LockGroupForRead(&lock, policy);
                if (r.outcome == cgdf::GroupLock::kQuickGaveUp) {
                    ++quick;
                } else if (r.outcome == cgdf::GroupLock::kGaveUp && MsSince(askStart) >= 80) {
                    fullAt = MsSince(start);
                    break;
                }
                Sleep(5);
            }
            std::printf("    %d quick give-ups, then a full wait at %.0f ms (limit 600)\n", quick, fullAt);
            Check(quick > 20 && fullAt >= 500 && fullAt < 800, "quick give-ups end at quickAnswerForMs");
        }

        // A walk asks about other groups in between: those are taken, and
        // the stuck one stays a quick give-up.
        {
            GameLock other{};
            Holder writer(lock, true);
            r = cgdf::LockGroupForRead(&lock, policy);
            Check(r.outcome == cgdf::GroupLock::kGaveUp, "gave up on lock 1");
            r = cgdf::LockGroupForRead(&other, policy);
            Check(r.outcome == cgdf::GroupLock::kAcquired && Is(other, 0, 1), "lock 2 unaffected");
            cgdf::UnlockForRead(&other);
            const auto start = std::chrono::steady_clock::now();
            r = cgdf::LockGroupForRead(&lock, policy);
            Check(r.outcome == cgdf::GroupLock::kQuickGaveUp && MsSince(start) < 20, "lock 1 still a quick give-up after asking about lock 2");
        }
        Check(Is(lock, 0, 0), "all free");
    }

    void TestReaderRaces()
    {
        Section("reader races with no wait allowed");
        GameLock lock{};
        std::atomic<bool> stop{ false };
        std::vector<std::thread> churn;
        for (int i = 0; i < 6; ++i) {
            churn.emplace_back([&, i] {
                while (!stop.load()) {
                    if (i % 2) {
                        g_game.lockForRead(&lock);
                        g_game.unlockForRead(&lock);
                    } else {
                        std::uint64_t w = 0;
                        if (cgdf::LockForReadWithin(&lock, 0, w)) {
                            cgdf::UnlockForRead(&lock);
                        }
                    }
                }
            });
        }
        int falseNew = 0;
        int falseLegacy = 0;
        constexpr int kTries = 300000;
        for (int i = 0; i < kTries; ++i) {
            std::uint64_t w = 0;
            if (cgdf::LockForReadWithin(&lock, 0, w)) {
                cgdf::UnlockForRead(&lock);
            } else {
                ++falseNew;
            }
            if (LegacyLockForReadWithin(&lock, 0)) {
                cgdf::UnlockForRead(&lock);
            } else {
                ++falseLegacy;
            }
        }
        stop = true;
        for (auto& t : churn) {
            t.join();
        }
        std::printf("    %d tries each, no writer: 1.1 failed %d, 1.0 failed %d\n", kTries, falseNew, falseLegacy);
        Check(falseNew == 0, "never fails with only readers around");
        Check(Is(lock, 0, 0), "count back to 0");
    }

    void TestImpossibleCount()
    {
        Section("a count the game never makes");
        GameLock lock{ 0, 0x10000000 };  // bits 28..30 set, no writer bit: the game's reader would spin forever
        const auto start = std::chrono::steady_clock::now();
        Check(!OurRead(lock, 50) && MsSince(start) >= 30, "ends at the limit instead of spinning");
        Check(Is(lock, 0, 0x10000000), "untouched");
    }

    void TestStress()
    {
        Section("stress: game writers, game readers, bounded readers");
        GameLock lock{};
        std::atomic<bool> stop{ false };
        std::atomic<int> writersIn{ 0 };
        std::atomic<int> readersIn{ 0 };
        std::atomic<long long> violations{ 0 };
        std::atomic<long long> writes{ 0 }, gameReads{ 0 }, ourReads{ 0 }, ourGiveUps{ 0 }, nested{ 0 };
        volatile long long a = 0;
        volatile long long b = 0;

        auto reading = [&] {
            ++readersIn;
            if (writersIn.load() != 0 || a != b) {
                ++violations;
            }
            --readersIn;
        };
        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([&] {
                while (!stop.load()) {
                    g_game.lockForWrite(&lock);
                    if (++writersIn != 1 || readersIn.load() != 0) {
                        ++violations;
                    }
                    a = a + 1;
                    if ((a & 7) == 0) {  // a nested read under the write, through ours
                        std::uint64_t w = 0;
                        if (cgdf::LockForReadWithin(&lock, 0, w)) {
                            ++nested;
                            cgdf::UnlockForRead(&lock);
                        } else {
                            ++violations;
                        }
                    }
                    b = a;
                    --writersIn;
                    g_game.unlockForWrite(&lock);
                    ++writes;
                    Sleep(0);
                }
            });
        }
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([&] {
                while (!stop.load()) {
                    g_game.lockForRead(&lock);
                    reading();
                    g_game.unlockForRead(&lock);
                    ++gameReads;
                }
            });
        }
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&, i] {
                const cgdf::GroupLockPolicy policy{ static_cast<std::uint64_t>(i), 50, 20 };  // 0..3 ms: give up often
                while (!stop.load()) {
                    const auto r = cgdf::LockGroupForRead(&lock, policy);
                    if (r.outcome == cgdf::GroupLock::kAcquired) {
                        reading();
                        cgdf::UnlockForRead(&lock);
                        ++ourReads;
                    } else {
                        ++ourGiveUps;
                    }
                }
            });
        }
        Sleep(3000);
        stop = true;
        for (auto& t : threads) {
            t.join();
        }
        std::printf("    3 s: %lld writes (%lld nested reads), %lld game reads, %lld bounded reads, %lld give-ups\n",
                    writes.load(), nested.load(), gameReads.load(), ourReads.load(), ourGiveUps.load());
        Check(violations == 0, "no reader alongside a writer, no two writers, no torn data");
        Check(writes > 0 && gameReads > 0 && ourReads > 0, "every kind made progress");
        Check(Is(lock, 0, 0), "lock word back to {0, 0}");
    }
}

int main()
{
    std::printf("Bounded read-lock vs a model of Skyrim AE 1.6.1170's BSReadWriteLock (a simulation, not the game)\n\n");
    TestSingleThreadEquivalence();
    TestUncontendedAndNested();
    TestMultipleReaders();
    TestActiveWriter();
    TestWriterReleasesDuringWait();
    TestWriterWaitsForUs();
    TestPolicy();
    TestReaderRaces();
    TestImpossibleCount();
    TestStress();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
