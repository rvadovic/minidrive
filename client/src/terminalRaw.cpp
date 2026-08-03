#include <termios.h>
#include <unistd.h>
#include "terminalRaw.hpp"

TerminalRaw::TerminalRaw() {
    tcgetattr(STDIN_FILENO, &old_);
    new_ = old_;
    new_.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    new_.c_cc[VMIN] = 1;
    new_.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_);
}

TerminalRaw::~TerminalRaw() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_);
}
