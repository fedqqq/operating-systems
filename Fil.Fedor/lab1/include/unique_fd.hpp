#pragma once

#include <unistd.h>

class UniqueFd {
public:
    explicit UniqueFd(int value = -1) : value_(value) {}
    ~UniqueFd() { if (value_ >= 0) ::close(value_); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    int get() const { return value_; }
private:
    int value_;
};
