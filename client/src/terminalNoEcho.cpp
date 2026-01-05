#include <termios.h>
#include <unistd.h>
#include "terminalNoEcho.hpp"

TerminalNoEcho::TerminalNoEcho() {
    tcgetattr(STDIN_FILENO, &old_);
    new_ = old_;
    new_.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_);
}

TerminalNoEcho::~TerminalNoEcho() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_);
}
