#pragma once

// Fingerprints of the game code this plugin depends on, as it is in Skyrim
// AE 1.6.1170 (SkyrimSE.exe 1.6.1170.0): each function's length and the
// FNV-1a 64-bit hash of its bytes. Install() hashes the live game's code and
// installs only if every fingerprint matches, so the plugin runs only on the
// exact code it was verified against -- without carrying any of the game's
// code itself. None of these ranges contain base relocations, so the bytes
// (and hashes) are the same wherever the game is loaded.
//
// What the fingerprinted functions do (docs/TECHNICAL.md has the evidence):
//
// ID 44706, is an actor in a combat group's target list
// (CombatGroup*, TESObjectREFR*) -> bool:
//   read-locks the group's BSReadWriteLock at +0x160 (call to 68233 at +0x2E),
//   gets the actor's handle (call to 37476 at +0x3C), scans the group's target
//   array (data at +0x08, count at +0x18, 0xA8-byte entries with the handle
//   first), unlocks (call to 68239 at +0x8C) and returns 0 or 1 in all of eax.
//
// ID 68233, BSReadWriteLock::LockForRead:
//   if this thread is the writer: lock-increment the count and return.
//   Otherwise swap (count & 0x0FFFFFFF) for one more; on a failed swap, retry
//   at once if the count it saw has no writer bit, else Sleep(0) up to 10000
//   times, then Sleep(1), and read the count again.
//
// ID 68234, BSReadWriteLock::LockForWrite:
//   if this thread is the writer: lock-increment the count and return.
//   Otherwise swap 0 (no readers, no writer) for 0x80000001, sleeping as above
//   between tries; then store this thread's id as the writer, with a fence.
//
// ID 68239, BSReadWriteLock::UnlockForRead: lock-decrement the count.
//
// ID 68240, BSReadWriteLock::UnlockForWrite:
//   if the count is 0x80000001: clear the writer, fence, exchange the count
//   for 0; otherwise lock-decrement the count.

#include <cstddef>
#include <cstdint>

namespace cgdf::code
{
    struct Fingerprint
    {
        std::size_t   size;
        std::uint64_t hash;  // FNV-1a 64 of the function's bytes
    };

    inline constexpr Fingerprint kIsGroupTarget{ 0xA6, 0x3239568B6498E700 };   // ID 44706
    inline constexpr Fingerprint kLockForRead{ 0x70, 0x9A7F003A00887109 };     // ID 68233
    inline constexpr Fingerprint kLockForWrite{ 0x7E, 0x9B15381233F14F43 };    // ID 68234
    inline constexpr Fingerprint kUnlockForRead{ 0x05, 0xB9FF0DCD992A3E6A };   // ID 68239
    inline constexpr Fingerprint kUnlockForWrite{ 0x1A, 0x2A3217348DDE53B5 };  // ID 68240

    // Where 44706 calls the functions it relies on (E8 rel32).
    inline constexpr std::size_t kIsGroupTargetCallLockForRead = 0x2E;
    inline constexpr std::size_t kIsGroupTargetCallGetHandle = 0x3C;
    inline constexpr std::size_t kIsGroupTargetCallUnlockForRead = 0x8C;

    [[nodiscard]] inline std::uint64_t Fnv1a64(const std::uint8_t* a_bytes, std::size_t a_size) noexcept
    {
        std::uint64_t hash = 0xCBF29CE484222325;
        for (std::size_t i = 0; i < a_size; ++i) {
            hash ^= a_bytes[i];
            hash *= 0x100000001B3;
        }
        return hash;
    }

    [[nodiscard]] inline bool Matches(std::uintptr_t a_address, const Fingerprint& a_fingerprint) noexcept
    {
        return Fnv1a64(reinterpret_cast<const std::uint8_t*>(a_address), a_fingerprint.size) == a_fingerprint.hash;
    }
}
