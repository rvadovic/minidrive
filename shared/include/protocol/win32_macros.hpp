#pragma once

// <windows.h> defines several macros whose names collide with protocol constants: DELETE (winnt.h,
// an access-right bit) and ERROR (wingdi.h). A macro is substituted before the compiler ever sees
// the namespace, so `protocol::commands::DELETE` becomes `protocol::commands::(0x00010000L)`.
//
// asio pulls <windows.h> into every Windows translation unit, in whatever order the includes
// happen to run. So the Windows headers are included *here*, first, and the offending macros are
// removed straight after. Include guards then stop any later #include from redefining them, which
// makes the fix independent of include order rather than dependent on it.
//
// ERROR is normally already absent: shared/CMakeLists.txt defines NOGDI on Windows. The #undef is
// a backstop for a consumer that includes this header without those compile definitions.
#ifdef _WIN32
#include <winsock2.h> // before any <windows.h>: winsock2.h has to win the race with winsock.h
#include <windows.h>
#ifdef DELETE
#undef DELETE
#endif
#ifdef ERROR
#undef ERROR
#endif
#endif
