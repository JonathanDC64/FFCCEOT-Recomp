// Crash reports: on a crash in the game's own code (an access violation and the like) or an abort (the runtime's
// fatal errors), writes a symbolized stack trace to stderr and to crash-<time>.txt, plus crash-<time>.dmp (a
// minidump), in the working directory. Windows only; elsewhere this does nothing.
#pragma once

namespace crash_report {

void install();

}  // namespace crash_report
