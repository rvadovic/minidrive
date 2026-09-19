#include "user_lock.hpp"

#include "storage.hpp"

UserLock::UserLock(std::shared_ptr<Storage> storage, std::string user, uint64_t owner)
    : storage_(std::move(storage)), user_(std::move(user)), owner_(owner) {
    if(storage_ == nullptr) return;
    held_ = storage_->try_acquire_user_lock(user_, owner_);
}

UserLock::~UserLock() {
    release();
}

UserLock::UserLock(UserLock&& other) noexcept
    : storage_(std::move(other.storage_)),
      user_(std::move(other.user_)),
      owner_(other.owner_),
      held_(other.held_) {
    other.held_ = false; // Exactly one object owns the lock at any moment
    other.owner_ = 0;
}

UserLock& UserLock::operator=(UserLock&& other) noexcept {
    if(this == &other) return *this;

    // Whatever this object was holding is released first. Without it, parking a new lock in a
    // member that already held one would silently strand the old one until the session ended.
    release();

    storage_ = std::move(other.storage_);
    user_ = std::move(other.user_);
    owner_ = other.owner_;
    held_ = other.held_;
    other.held_ = false;
    other.owner_ = 0;
    return *this;
}

void UserLock::release() {
    if(!held_) return;
    held_ = false;
    if(storage_ != nullptr) {
        storage_->release_user_lock(user_, owner_);
    }
}
