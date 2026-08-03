#pragma once

#include <termios.h>
#include <unistd.h>

// Puts the terminal into non-canonical, non-echo mode (ISIG left enabled so
// Ctrl-C/Ctrl-Z keep working) so the client can read and handle individual
// keystrokes itself (arrow keys, backspace, etc.) instead of relying on the
// kernel's line discipline. Restores the original settings on destruction.
class TerminalRaw {
public:
    TerminalRaw();
    ~TerminalRaw();

private:
    termios old_, new_;
};
