#pragma once

#include <termios.h>
#include <unistd.h>

class TerminalNoEcho {
public:
    TerminalNoEcho(); // Sets echo in cmd
    ~TerminalNoEcho(); // Resets echo in cmd

private:
    termios old_, new_;
};