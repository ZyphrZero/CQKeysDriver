#pragma once
#include "../shared/cqkeys_protocol.h"
#include <stddef.h>

namespace cq {
enum class Result { Ok, InvalidLength, InvalidInput, Unsupported, Full };

struct Report {
    uint8_t Data[10];
    uint8_t Length;
};

struct InputState {
    uint8_t Modifiers;
    uint8_t Keys[6];
    uint8_t Buttons;
    int32_t WheelRemainder;
    int32_t HorizontalRemainder;
};

struct Batch {
    Report Reports[2];
    unsigned Count;
    InputState NextState;
};

// Pure translation: never changes the committed state or sends input.
Result PrepareInput(uint32_t code, const void* data, size_t size,
                    const InputState& state, Batch& batch);
Batch ReleaseAll();
}
