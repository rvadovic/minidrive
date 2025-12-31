#pragma once

#include <termios.h>
#include <unistd.h>

class TerminalNoEcho {
public:
    TerminalNoEcho();
    ~TerminalNoEcho();

private:
    termios old_, new_;
};