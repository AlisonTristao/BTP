#ifndef BTP_PROFILES_LOOP_HPP
#define BTP_PROFILES_LOOP_HPP

// Names and enum values of the `loop` profile (closed-loop control).
// Specification: docs/profiles/loop.md.
//
// A profile is a naming/semantic CONVENTION layered on BTP. It is NOT part of
// the protocol: an endpoint need not follow it, BTP can be used in any other
// way, and nothing in the BTP library depends on this header. It only gives
// the profile's strings and values one spelling, for code that chooses to use
// them. Consumers bind by NAME (topic + field, action + parameter), never by
// numeric id -- ids are the producer's choice.
//
// Header-only, C++11, standard library not required (only <cstdint>).

#include <cstdint>

namespace btp {
namespace profiles {
namespace loop {

// Reserved name prefix of this profile.
constexpr const char* kPrefix = "loop.";

// ---- Telemetry topics (plant -> controller) ---------------------------------

namespace topic {
constexpr const char* kState  = "loop.state";   // required
constexpr const char* kOutput = "loop.output";  // required
constexpr const char* kStats  = "loop.stats";   // optional, low rate
}  // namespace topic

// ---- Actions (controller -> plant) -------------------------------------------

namespace action {
constexpr const char* kU      = "loop.u";       // STREAM
constexpr const char* kInject = "loop.inject";  // STREAM, INJECT mode only
constexpr const char* kMode   = "loop.mode";    // regular, DANGEROUS
constexpr const char* kConfig = "loop.config";  // regular, IDEMPOTENT
}  // namespace action

// ---- Field and parameter names -----------------------------------------------
//
// Application-defined fields (the measurements of loop.state / loop.inject and
// the actuation of loop.output / loop.u) have no fixed name and are not listed.

namespace field {
// loop.state
constexpr const char* kTick = "tick";  // u32, the plant's own loop counter
constexpr const char* kMode = "mode";  // enum8, see Mode
// loop.output
constexpr const char* kRefTick = "ref_tick";  // u32 (also a loop.u parameter)
constexpr const char* kSource  = "source";    // enum8, see Source
// loop.stats
constexpr const char* kRx      = "rx";        // u32
constexpr const char* kLate    = "late";      // u32
constexpr const char* kLost    = "lost";      // u32
constexpr const char* kAgeLast = "age_last";  // u16, ticks
constexpr const char* kPeriodUs = "period_us";  // u32
}  // namespace field

namespace param {
// loop.u
constexpr const char* kRefTick = "ref_tick";  // u32
// loop.inject (same name as the loop.state field)
constexpr const char* kTick = "tick";  // u32
// loop.mode
constexpr const char* kMode = "mode";  // enum8, see Mode
constexpr const char* kArm  = "arm";   // bool
}  // namespace param

// ---- Enumerations ------------------------------------------------------------

// loop.state.mode and loop.mode.mode
enum Mode : std::uint8_t {
  kModeOff    = 0,  // no actuation
  kModeLocal  = 1,  // the plant runs its own controller
  kModeRemote = 2,  // actuation comes from loop.u
  kModeInject = 3   // measurements come from loop.inject (plant-in-the-loop)
};

// loop.output.source. Values >= 3 are reserved for a future loss policy.
enum Source : std::uint8_t {
  kSourceNone   = 0,
  kSourceLocal  = 1,
  kSourceRemote = 2
};

}  // namespace loop
}  // namespace profiles
}  // namespace btp

#endif  // BTP_PROFILES_LOOP_HPP
