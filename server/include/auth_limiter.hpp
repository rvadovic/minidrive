#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Lockout for repeated password failures on the TCP AUTH path.
//
// Before this, session.cpp accepted unlimited password retries with no delay and no lockout, on a
// connection an attacker can keep open indefinitely. That mattered before the vault and matters
// more after it: the password no longer merely opens a session, it derives MK, which unwraps VK,
// which unwraps every file's DEK. An online guessing oracle against that password is an oracle
// against the entire vault.
//
// **State is per username and shared by every session**, which is the only arrangement that helps:
// a counter living on the Session would reset on reconnect, and reconnecting is free.
//
// The lockout is temporary and escalating rather than permanent, because a per-username lockout is
// also a denial-of-service primitive against the legitimate owner of that username - an attacker
// who knows a name can lock it. Three properties bound that damage:
//
//   1. Lockouts expire on their own; nothing needs an administrator to clear them.
//   2. **Attempts made during a lockout do not extend it.** check() is consulted before the
//      password is ever verified, so a rejected attempt never reaches record_failure(). Without
//      this, an attacker holding a connection open and guessing in a loop would keep a victim
//      locked out permanently, which is a worse bug than the one being fixed.
//   3. Escalation decays: a user who eventually gets in, or who simply stays quiet past the decay
//      window, starts again from the shortest lockout.
class AuthLimiter {
public:
    struct Decision {
        bool allowed = true;
        uint32_t retry_after_seconds = 0; // Meaningful only when allowed is false
    };

    // Defaults: 5 failures within 5 minutes locks the account for 30s, doubling per consecutive
    // lockout up to 15 minutes. Slow enough to make online guessing useless against even a weak
    // password, short enough that a legitimate user who fatfingers their password five times is
    // inconvenienced rather than locked out of their data.
    explicit AuthLimiter(size_t max_failures = 5,
                         std::chrono::seconds window = std::chrono::minutes(5),
                         std::chrono::seconds base_lockout = std::chrono::seconds(30),
                         std::chrono::seconds max_lockout = std::chrono::minutes(15));

    // Consulted *before* the password is checked. Never mutates the lockout deadline - see (2).
    Decision check(const std::string& username);

    void record_failure(const std::string& username); // Only ever called for an attempt check() allowed
    void record_success(const std::string& username); // Clears the record for that user

private:
    struct Record {
        size_t failures = 0; // Within the current window
        std::chrono::steady_clock::time_point first_failure{};
        std::chrono::steady_clock::time_point locked_until{};
        size_t consecutive_lockouts = 0; // Drives the escalation
        std::chrono::steady_clock::time_point last_activity{};
    };

    const size_t max_failures_;
    const std::chrono::seconds window_;
    const std::chrono::seconds base_lockout_;
    const std::chrono::seconds max_lockout_;

    std::mutex mutex_;
    std::unordered_map<std::string, Record> records_;

    void prune(std::chrono::steady_clock::time_point now); // Called under mutex_
};
