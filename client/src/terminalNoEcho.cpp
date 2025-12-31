#include <termios.h>
#include <unistd.h>
#include "terminalNoEcho.hpp"

TerminalNoEcho::TerminalNoEcho() {
    tcgetattr(STDIN_FILENO, &old_);
        new_ = old_;
        new_.c_cflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &new_);
}

TerminalNoEcho::~TerminalNoEcho() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_);
}
