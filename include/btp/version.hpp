#ifndef BTP_VERSION_HPP
#define BTP_VERSION_HPP

// The one place the BTP version number lives.
//
// BTP is ONE SemVer line -- MAJOR.MINOR.PATCH -- and everything else that
// looks like a separate "version" is derived from it:
//
//   MAJOR  The wire format changed incompatibly. Only this bumps MAJOR, so
//          MAJOR is also the newest wire-version byte (docs/frame.md section 2)
//          and the maintenance-branch name (MAJOR.x). `main` always carries
//          the highest MAJOR; a superseded one lives on its MAJOR.x branch.
//   MINOR  A backward-compatible addition -- a new library layer, an optional
//          field. The wire is unchanged.
//   PATCH  A correction with no effect on the wire or the public API.
//
// The git tag is vMAJOR.MINOR.PATCH -- the same number. There is no separate
// "library version": the tag, the release and this header are one thing.
//
// Single source of truth. Every other copy of the number is generated or
// checked from these three lines:
//   * CMakeLists.txt parses them for project(VERSION);
//   * library.json (which PlatformIO reads, and which never sees CMake) carries
//     a copy that `python tools/version.py` keeps in step, and that a plain
//     CMake configure re-checks and refuses to build on a mismatch.
//
// To release: `python tools/version.py X.Y.Z`, commit, then `git tag vX.Y.Z`.

#include <cstdint>

namespace btp {

// The release / library version. Public since 2.2.0.
static const std::uint8_t kLibraryVersionMajor = 2U;
static const std::uint8_t kLibraryVersionMinor = 27U;
static const std::uint8_t kLibraryVersionPatch = 0U;

// The wire-version byte at header offset 4 (docs/frame.md section 2). It is a
// frame-format tag, not a release number:
//   1  the base frame;
//   2  the payload is AEAD-sealed -- ENCRYPTED set (docs/encryption.md).
// A library on the MAJOR-N line encodes the lowest tag a frame needs and
// decodes every tag in [kMinimumProtocolVersion, kMaximumProtocolVersion].
// The ceiling equals the MAJOR by the rule above: a new wire tag is exactly
// what a MAJOR bump is.
static const std::uint8_t kMinimumProtocolVersion = 1U;
static const std::uint8_t kMaximumProtocolVersion = 2U;

static_assert(kMaximumProtocolVersion == kLibraryVersionMajor,
              "a new wire version is a MAJOR change: bump kLibraryVersionMajor "
              "with kMaximumProtocolVersion, or cut the previous major to its "
              "MAJOR.x branch");
static_assert(kMinimumProtocolVersion >= 1U &&
                  kMinimumProtocolVersion <= kMaximumProtocolVersion,
              "the accepted wire-version range is inverted");

}  // namespace btp

#endif  // BTP_VERSION_HPP
