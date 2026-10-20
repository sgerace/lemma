#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace lemma {

// A parse or evaluation error. It refers to the byte range of parsed text, or to the
// MathNode (by id) of a math region's tree.
class Error : public std::runtime_error {
public:
    Error(const std::string& message, std::size_t begin = 0, std::size_t end = 0, int node = 0)
        : std::runtime_error(message), begin_(begin), end_(end), node_(node) {}

    std::size_t begin() const { return begin_; }
    std::size_t end() const { return end_; }
    int node() const { return node_; }

private:
    std::size_t begin_;
    std::size_t end_;
    int node_;
};

} // namespace lemma
