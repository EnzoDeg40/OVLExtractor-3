#pragma once

#include <cstddef>
#include <cstdint>

namespace ovl {

// The `.CHK` sidecar that sits next to some OVLs (Main.common.ovl.CHK,
// Style.unique.ovl.CHK, …) holds a single little-endian u32: a CRC-32 of the
// OVL, computed by RCT3.exe at 0x00E04190.
//
// It is standard CRC-32 (poly 0x04C11DB7 reflected, init 0xFFFFFFFF, final
// complement) applied to the file contents **followed by a repeat of the file's
// last byte**. That trailing byte is not a design choice: the game's read loop
// tests `feof` only after folding a byte in, so the final `fread` fails at EOF,
// leaves the previous byte in its one-byte stack buffer, and folds it a second
// time. Any reimplementation has to reproduce it to agree with shipped files.
//
// See docs/RCT3_OVL_FORMAT.md §14 for the disassembly this was recovered from.

// Checksum of a whole OVL image, matching what the game writes to `.CHK`.
// `size == 0` returns 0: the game reads an uninitialised stack byte in that
// case, so there is no value worth reproducing.
std::uint32_t chk_checksum(const std::byte* data, std::size_t size) noexcept;

// Convenience wrapper: does `stored` (as read from a `.CHK`) match `data`?
bool chk_matches(const std::byte* data, std::size_t size,
                 std::uint32_t stored) noexcept;

}  // namespace ovl
