// tests/test_qt_macro_safety.cpp
//
// Simulates the macro environment Qt's <QObject> installs (unless a consumer
// builds with QT_NO_KEYWORDS): slots/signals/emit become macros, and in a
// default Qt build QT_NO_KEYWORDS is NOT defined, so any BTP public header a
// Qt application includes sees these expansions too.
//
//   #define slots        Q_SLOTS      // -> nothing
//   #define signals      Q_SIGNALS    // -> public
//   #define emit         Q_EMIT       // -> nothing
//
// A BTP header with a parameter or, worse, a MEMBER actually named `slots`
// (etc.) does not just get a confusing name here -- the token vanishes
// (`ReassemblySlot slots[Slots];` becomes `ReassemblySlot [Slots];`, a hard
// parse error) or turns into a class-defining keyword. This is not
// hypothetical: it is exactly what broke the first real Qt consumer
// (TraceView, library 2.38.0) the moment it included btp/node.hpp. Every
// constructor this test exercises is one that was fixed then -- see each
// one's own "slot_array, not slots" comment.
//
// No assertions and no Qt dependency (defining the macros by hand is enough
// to reproduce the failure) -- this test's only job is to compile. If it
// fails to compile, a public header reintroduced one of these identifiers.

#define slots
#define signals public
#define emit

#include <btp/catalog.hpp>
#include <btp/fragmentation.hpp>
#include <btp/messages.hpp>
#include <btp/node.hpp>
#include <btp/receiver.hpp>
#include <btp/session.hpp>
#include <btp/subscription.hpp>

int main() { return 0; }
