#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Storage;

// RAII over Storage's per-user busy flag.
//
// The lock is not reentrant and it is not a queue: a second request for the same user gets an
// immediate "Server is busy" (see "Conventions" in CLAUDE.md). That makes a handler which returns
// without releasing far worse than it sounds - every *subsequent* command from that user fails the
// same way, forever, and from the client side it is indistinguishable from a deadlock. It survives
// until the next server restart. There were around fifty hand-written release paths, each one an
// independent chance to introduce that.
//
// Hold this instead and the release cannot be forgotten. Two shapes are in use:
//
//   UserLock lock = acquire_lock();      // synchronous handler: every return path releases
//   if(!lock) { ...busy... return; }
//
//   lock_ = std::move(lock);             // a transfer keeps it across async boundaries, and the
//                                        // terminal handler calls lock_.release()
//
// Deliberately constructed only through Session::acquire_lock(), which is what supplies the owner
// id. Bug #11 made that id the thing which distinguishes the holder from any other session of the
// same user; a second place that constructs locks is a second place that can get it wrong.
class UserLock {
public:
    UserLock() = default; // Holds nothing
    UserLock(std::shared_ptr<Storage> storage, std::string user, uint64_t owner); // Tries to take it
    ~UserLock();

    // Movable so a handler can hand its lock to a transfer that outlives the handler; never
    // copyable, because two objects releasing one lock is the bug this class exists to remove.
    UserLock(UserLock&& other) noexcept;
    UserLock& operator=(UserLock&& other) noexcept;
    UserLock(const UserLock&) = delete;
    UserLock& operator=(const UserLock&) = delete;

    bool owns() const { return held_; }
    explicit operator bool() const { return held_; }

    void release(); // Early release; a no-op when nothing is held, so it is safe on any path

private:
    std::shared_ptr<Storage> storage_;
    std::string user_;
    uint64_t owner_ = 0;
    bool held_ = false;
};
