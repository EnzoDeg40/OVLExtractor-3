#pragma once

#include <stdexcept>
#include <string>

namespace ovl {

class OvlError : public std::runtime_error {
public:
    explicit OvlError(const std::string& msg) : std::runtime_error(msg) {}
    explicit OvlError(const char* msg) : std::runtime_error(msg) {}
};

}  // namespace ovl
