#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clip_worker::formats {

class ByteView {
public:
    ByteView() = default;

    explicit ByteView(const std::vector<std::uint8_t>& bytes)
        : data_(bytes.data()), size_(bytes.size()) {
    }

    ByteView(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {
    }

    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool contains(std::size_t offset, std::size_t length) const noexcept {
        return offset <= size_ && length <= size_ - offset;
    }

    [[nodiscard]] ByteView subview(std::size_t offset, std::size_t length) const noexcept {
        return contains(offset, length) ? ByteView(data_ + offset, length) : ByteView();
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace clip_worker::formats

