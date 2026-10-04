/**
 * @file LittleEndian.h
 * @author Jackey Cen
 * @brief Byte-wise little-endian stores and loads for the terrain pack format.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mir::detail
{
    // Byte by byte, independent of host order.
    class LeWriter
    {
    public:
        explicit LeWriter(std::vector<std::byte> &out) : mOut(out) {}

        size_t Size() const { return mOut.size(); }

        void U32(uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                mOut.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
        }
        void U64(uint64_t v)
        {
            for (int i = 0; i < 8; ++i)
                mOut.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
        }
        void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
        void F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
        void F64(double v) { U64(std::bit_cast<uint64_t>(v)); }
        void Bytes(std::span<const std::byte> bytes) { mOut.insert(mOut.end(), bytes.begin(), bytes.end()); }
        void Zeros(size_t count) { mOut.insert(mOut.end(), count, std::byte{0}); }

        void PadTo(uint64_t alignment)
        {
            const uint64_t rem = mOut.size() % alignment;
            if (rem != 0)
                Zeros(static_cast<size_t>(alignment - rem));
        }

    private:
        std::vector<std::byte> &mOut;
    };

    // Unchecked: callers bounds-check the whole structure first.
    inline uint32_t LoadU32(std::span<const std::byte> b, size_t at)
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(std::to_integer<uint8_t>(b[at + i])) << (8 * i);
        return v;
    }
    inline uint64_t LoadU64(std::span<const std::byte> b, size_t at)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(std::to_integer<uint8_t>(b[at + i])) << (8 * i);
        return v;
    }
    inline int32_t LoadI32(std::span<const std::byte> b, size_t at) { return static_cast<int32_t>(LoadU32(b, at)); }
    inline float LoadF32(std::span<const std::byte> b, size_t at) { return std::bit_cast<float>(LoadU32(b, at)); }
    inline double LoadF64(std::span<const std::byte> b, size_t at) { return std::bit_cast<double>(LoadU64(b, at)); }
}
