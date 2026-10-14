#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace lemma {

// A parse or evaluation error, with the byte range of the source it refers to.
class Error : public std::runtime_error {
public:
    Error(const std::string& message, std::size_t begin = 0, std::size_t end = 0)
        : std::runtime_error(message), begin_(begin), end_(end) {}

    std::size_t begin() const { return begin_; }
    std::size_t end() const { return end_; }

private:
    std::size_t begin_;
    std::size_t end_;
};

} // namespace lemma
