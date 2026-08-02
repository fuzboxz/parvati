// Faithful C++17 port of Ambika's `controller/note_stack.h`.
//
// Original: ambika_reference/controller/note_stack.h (Emilie Gillet, GPL3).
//
// A pre-allocated linked-list + sorted-array note stack used for monophonic
// note priority and arpeggiation. The algorithm is byte-for-byte identical to
// the firmware (1-based pool indices, sorted-by-pitch side array). Only the
// C++ scaffolding (namespacing, constexpr, delete-copy) differs.

#ifndef PARVATI_NOTE_STACK_H_
#define PARVATI_NOTE_STACK_H_

#include <cstdint>
#include <cstring>

namespace parvati {

static constexpr uint8_t kFreeSlot = 0xff;

struct NoteEntry
{
    uint8_t note;
    uint8_t velocity;
    uint8_t next_ptr;  // Base 1 (0 = end of list)
};

template <uint8_t capacity>
class NoteStack
{
public:
    NoteStack() = default;

    void init() { clear(); }

    void noteOn (uint8_t note, uint8_t velocity)
    {
        // Remove the note first (in case it is already here).
        noteOff (note);
        // Saturation: remove the least recently played note.
        if (size_ == capacity)
        {
            uint8_t least_recent_note = 0;
            for (uint8_t i = 1; i <= capacity; ++i)
                if (pool_[i].next_ptr == 0)
                    least_recent_note = pool_[i].note;
            noteOff (least_recent_note);
        }
        // Find a free slot.
        uint8_t free_slot = 0;
        for (uint8_t i = 1; i <= capacity; ++i)
        {
            if (pool_[i].note == kFreeSlot)
            {
                free_slot = i;
                break;
            }
        }
        pool_[free_slot].next_ptr = root_ptr_;
        pool_[free_slot].note = note;
        pool_[free_slot].velocity = velocity;
        root_ptr_ = free_slot;
        // Insert into the sorted list.
        for (uint8_t i = 0; i < size_; ++i)
        {
            if (pool_[sorted_ptr_[i]].note > note)
            {
                for (uint8_t j = size_; j > i; --j)
                    sorted_ptr_[j] = sorted_ptr_[j - 1];
                sorted_ptr_[i] = free_slot;
                free_slot = 0;
                break;
            }
        }
        if (free_slot)
            sorted_ptr_[size_] = free_slot;
        ++size_;
    }

    void noteOff (uint8_t note)
    {
        uint8_t current = root_ptr_;
        uint8_t previous = 0;
        while (current)
        {
            if (pool_[current].note == note)
                break;
            previous = current;
            current = pool_[current].next_ptr;
        }
        if (current)
        {
            if (previous)
                pool_[previous].next_ptr = pool_[current].next_ptr;
            else
                root_ptr_ = pool_[current].next_ptr;
            for (uint8_t i = 0; i < size_; ++i)
            {
                if (sorted_ptr_[i] == current)
                {
                    for (uint8_t j = i; j < size_ - 1; ++j)
                        sorted_ptr_[j] = sorted_ptr_[j + 1];
                    break;
                }
            }
            pool_[current].next_ptr = 0;
            pool_[current].note = kFreeSlot;
            pool_[current].velocity = 0;
            --size_;
        }
    }

    void clear()
    {
        size_ = 0;
        std::memset (pool_ + 1, 0, sizeof (NoteEntry) * capacity);
        std::memset (sorted_ptr_ + 1, 0, capacity);
        root_ptr_ = 0;
        for (uint8_t i = 0; i <= capacity; ++i)
            pool_[i].note = kFreeSlot;
    }

    uint8_t size() const { return size_; }
    uint8_t max_size() const { return capacity; }

    const NoteEntry& most_recent_note() const { return pool_[root_ptr_]; }

    const NoteEntry& sorted_note (uint8_t index) const
    {
        return pool_[sorted_ptr_[index]];
    }

    // The n-th note in played (LIFO) order, i.e. most-recent-first reversed.
    const NoteEntry& played_note (uint8_t index) const
    {
        uint8_t current = root_ptr_;
        index = size_ - index - 1;
        for (uint8_t i = 0; i < index; ++i)
            current = pool_[current].next_ptr;
        return pool_[current];
    }

private:
    uint8_t size_ = 0;
    NoteEntry pool_[capacity + 1] {};   // Index 0 is a dummy node.
    uint8_t root_ptr_ = 0;              // Base 1.
    uint8_t sorted_ptr_[capacity + 1] {};

    NoteStack (const NoteStack&) = delete;
    NoteStack& operator= (const NoteStack&) = delete;
};

}  // namespace parvati

#endif  // PARVATI_NOTE_STACK_H_
