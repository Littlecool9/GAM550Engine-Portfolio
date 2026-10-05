/**
 * @file Slots.h
 * @author Sumin Park
 * @brief Slot/generation bookkeeping shared by the resource and material stores.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <vector>

namespace mir::vk
{
    // A handle resolves only while its slot still has the handle's generation.
    template <typename Slot, typename Handle>
    const Slot *FindSlot(const std::vector<Slot> &slots, Handle handle)
    {
        if (handle.IsNull() || handle.mIndex >= slots.size())
            return nullptr;

        const Slot &slot = slots[handle.mIndex];
        return slot.mGeneration == handle.mGeneration ? &slot : nullptr;
    }

    // Reuses a freed slot, keeping the generation RetireSlot bumped; otherwise appends at generation 1.
    template <typename Slot>
    uint32_t PlaceSlot(std::vector<Slot> &slots, std::vector<uint32_t> &freeIndices, Slot slot)
    {
        if (!freeIndices.empty())
        {
            const uint32_t index = freeIndices.back();
            freeIndices.pop_back();
            slot.mGeneration = slots[index].mGeneration;
            slots[index] = slot;
            return index;
        }

        slot.mGeneration = 1;
        slots.push_back(slot);
        return static_cast<uint32_t>(slots.size() - 1);
    }

    // Empties the slot and bumps its generation, so every outstanding handle to it goes stale.
    template <typename Slot>
    void RetireSlot(Slot &slot)
    {
        const uint32_t nextGeneration = slot.mGeneration + 1;
        slot = Slot{};
        slot.mGeneration = nextGeneration;
    }
}
