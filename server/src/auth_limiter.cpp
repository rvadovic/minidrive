#include "auth_limiter.hpp"

#include <algorithm>

namespace {
// How long a quiet user's record is kept before it is forgotten entirely. Also what makes the
// escalation decay: come back after this and the next lockout starts from base_lockout again.
constexpr auto RECORD_LIFETIME = std::chrono::hours(1);
} // namespace

AuthLimiter::AuthLimiter(size_t max_failures,
                         std::chrono::seconds window,
                         std::chrono::seconds base_lockout,
                         std::chrono::seconds max_lockout)
    : max_failures_(max_failures == 0 ? 1 : max_failures),
      window_(window),
      base_lockout_(base_lockout),
      max_lockout_(max_lockout) {}

void AuthLimiter::prune(std::chrono::steady_clock::time_point now) {
    for(auto it = records_.begin(); it != records_.end();) {
        const bool locked = now < it->second.locked_until;
        if(!locked && now - it->second.last_activity > RECORD_LIFETIME) {
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
}

AuthLimiter::Decision AuthLimiter::check(const std::string& username) {
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = records_.find(username);
    if(it == records_.end()) return Decision{true, 0};

    Record& record = it->second;
    if(now < record.locked_until) {
        // Deliberately does not touch locked_until or last_activity. An attempt made during a
        // lockout must not extend it, or an attacker guessing in a loop keeps the real owner
        // locked out indefinitely.
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(record.locked_until - now);
        return Decision{false, static_cast<uint32_t>(std::max<int64_t>(1, remaining.count()))};
    }

    return Decision{true, 0};
}

void AuthLimiter::record_failure(const std::string& username) {
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    prune(now);

    Record& record = records_[username];
    record.last_activity = now;

    // A lockout that has just expired ends the previous burst: the counter starts over, but the
    // escalation level is kept so that immediately resuming the attack locks out for longer.
    if(record.locked_until != std::chrono::steady_clock::time_point{} && now >= record.locked_until) {
        record.locked_until = {};
        record.failures = 0;
    }

    // Failures only count together if they land inside one window
    if(record.failures == 0 || now - record.first_failure > window_) {
        record.failures = 0;
        record.first_failure = now;
    }

    record.failures++;
    if(record.failures < max_failures_) return;

    // Threshold reached: lock, escalating on each consecutive lockout, capped.
    auto lockout = base_lockout_;
    for(size_t i = 0; i < record.consecutive_lockouts && lockout < max_lockout_; ++i) {
        lockout *= 2;
    }
    lockout = std::min(lockout, max_lockout_);

    record.locked_until = now + lockout;
    record.consecutive_lockouts++;
    record.failures = 0; // The burst has been answered; the next one starts fresh
}

void AuthLimiter::record_success(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Everything is forgotten, escalation included. A caller who proves the password is not the
    // attacker this exists to slow down.
    records_.erase(username);
}
