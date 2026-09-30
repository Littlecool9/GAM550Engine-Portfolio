/**
 * @file UIId.h
 * @author Quy Dong Ta
 * @brief A widget's identity across frames: a hash of a name plus an optional
 *        index, so a widget drawn in a loop needs no id kept at its call site.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace mir
{

    class UIId
    {
    public:
        constexpr UIId() = default;

        constexpr explicit UIId(std::string_view name) : mValue(NonNull(HashName(name))) {}

        constexpr UIId(std::string_view name, uint64_t index) : mValue(NonNull(Mix(HashName(name) ^ Mix(index)))) {}

        constexpr uint64_t Value() const { return mValue; }
        constexpr bool IsNull() const { return mValue == 0; }
        constexpr bool operator==(const UIId &) const = default;

    private:
    
        static constexpr uint64_t HashName(std::string_view name)
        {
            uint64_t hash = 14695981039346656037ull;
            for (const char c : name)
            {
                hash ^= static_cast<uint8_t>(c);
                hash *= 1099511628211ull;
            }
            return hash;
        }


        static constexpr uint64_t Mix(uint64_t value)
        {
            value += 0x9E3779B97F4A7C15ull;
            value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
            value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
            return value ^ (value >> 31);
        }

        static constexpr uint64_t NonNull(uint64_t value) { return value == 0 ? 1 : value; }

        uint64_t mValue = 0;
    };
} // namespace mir
