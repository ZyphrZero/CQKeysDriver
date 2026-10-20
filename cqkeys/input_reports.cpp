#include "input_reports.h"
#include <string.h>

namespace cq {
static uint8_t ScanToUsage(uint16_t scan, uint16_t flags)
{
    if (flags & KEY_E1) return scan == 0x45 ? 0x48 : 0; // Pause
    if (flags & KEY_E0) {
        switch (scan) {
        case 0x1c: return 0x58; // Keypad Enter
        case 0x1d: return 0xe4;
        case 0x35: return 0x54;
        case 0x37: return 0x46;
        case 0x38: return 0xe6;
        case 0x47: return 0x4a;
        case 0x48: return 0x52;
        case 0x49: return 0x4b;
        case 0x4b: return 0x50;
        case 0x4d: return 0x4f;
        case 0x4f: return 0x4d;
        case 0x50: return 0x51;
        case 0x51: return 0x4e;
        case 0x52: return 0x49;
        case 0x53: return 0x4c;
        case 0x5b: return 0xe3;
        case 0x5c: return 0xe7;
        case 0x5d: return 0x65;
        default: return 0;
        }
    }
    // PC set-1 scan codes -> USB HID Keyboard/Keypad usage IDs.
    static const uint8_t map[] = {
        0,0x29,0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x2d,0x2e,0x2a,0x2b,
        0x14,0x1a,0x08,0x15,0x17,0x1c,0x18,0x0c,0x12,0x13,0x2f,0x30,0x28,0xe0,0x04,0x16,
        0x07,0x09,0x0a,0x0b,0x0d,0x0e,0x0f,0x33,0x34,0x35,0xe1,0x31,0x1d,0x1b,0x06,0x19,
        0x05,0x11,0x10,0x36,0x37,0x38,0xe5,0x55,0xe2,0x2c,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,
        0x3f,0x40,0x41,0x42,0x43,0x53,0x47,0x5f,0x60,0x61,0x56,0x5c,0x5d,0x5e,0x57,0x59,
        0x5a,0x5b,0x62,0x63,0,0,0x64,0x44,0x45
    };
    return scan < sizeof(map) ? map[scan] : 0;
}

static void Put16(Report& report, unsigned offset, int32_t value)
{
    const uint16_t bits = static_cast<uint16_t>(value);
    report.Data[offset] = static_cast<uint8_t>(bits);
    report.Data[offset + 1] = static_cast<uint8_t>(bits >> 8);
}

static Report KeyboardReport(const InputState& state)
{
    Report report = {};
    report.Length = 9;
    report.Data[0] = 1;
    report.Data[1] = state.Modifiers;
    memcpy(report.Data + 3, state.Keys, sizeof(state.Keys));
    return report;
}

static Result Keyboard(const CQ_KEYBOARD_INPUT& input, Batch& batch)
{
    if ((input.Flags & ~(KEY_BREAK | KEY_E0 | KEY_E1)) ||
        (input.Flags & (KEY_E0 | KEY_E1)) == (KEY_E0 | KEY_E1) || input.Reserved)
        return Result::InvalidInput;
    if (input.UnitId || input.ExtraInformation) return Result::Unsupported;
    const uint8_t usage = ScanToUsage(input.MakeCode, input.Flags);
    if (!usage) return Result::Unsupported;
    InputState& next = batch.NextState;
    const bool released = (input.Flags & KEY_BREAK) != 0;
    if (usage >= 0xe0 && usage <= 0xe7) {
        const uint8_t mask = static_cast<uint8_t>(1u << (usage - 0xe0));
        if (released) next.Modifiers &= static_cast<uint8_t>(~mask);
        else next.Modifiers |= mask;
    } else {
        unsigned freeSlot = 6;
        bool present = false;
        for (unsigned i = 0; i < 6; ++i) {
            if (next.Keys[i] == usage) {
                present = true;
                if (released) next.Keys[i] = 0;
            }
            if (next.Keys[i] == 0) freeSlot = i;
        }
        if (!released && !present) {
            if (freeSlot == 6) return Result::Full;
            // Keep a deterministic first-free ordering in reports.
            for (freeSlot = 0; next.Keys[freeSlot] != 0; ++freeSlot) {}
            next.Keys[freeSlot] = usage;
        }
    }
    batch.Reports[0] = KeyboardReport(next);
    batch.Count = 1;
    return Result::Ok;
}

static Result Mouse(const CQ_MOUSE_INPUT& input, Batch& batch)
{
    if (input.UnitId || input.RawButtons || input.ExtraInformation ||
        (input.Flags & ~MOUSE_MOVE_ABSOLUTE)) return Result::Unsupported;
    if ((input.ButtonFlags & ~0x0fffu) ||
        (input.ButtonFlags & (MOUSE_WHEEL | MOUSE_HWHEEL)) == (MOUSE_WHEEL | MOUSE_HWHEEL))
        return Result::InvalidInput;
    if (!(input.ButtonFlags & (MOUSE_WHEEL | MOUSE_HWHEEL)) && input.ButtonData)
        return Result::InvalidInput;
    const bool absolute = (input.Flags & MOUSE_MOVE_ABSOLUTE) != 0;
    if (absolute) {
        if (input.LastX < 0 || input.LastX > 65535 || input.LastY < 0 || input.LastY > 65535)
            return Result::InvalidInput;
    } else if (input.LastX < -32768 || input.LastX > 32767 ||
               input.LastY < -32768 || input.LastY > 32767) {
        return Result::InvalidInput;
    }
    for (unsigned i = 0; i < 5; ++i) {
        const unsigned pair = (input.ButtonFlags >> (i * 2)) & 3u;
        if (pair == 3) return Result::InvalidInput;
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if (pair == 1) batch.NextState.Buttons |= bit;
        if (pair == 2) batch.NextState.Buttons &= static_cast<uint8_t>(~bit);
    }
    if (absolute) {
        Report& position = batch.Reports[batch.Count++];
        position.Length = 6;
        position.Data[0] = 3;
        // Submit the position before any accompanying button transition.
        Put16(position, 2, input.LastX);
        Put16(position, 4, input.LastY);
    }
    if (!absolute || input.ButtonFlags) {
        Report& relative = batch.Reports[batch.Count++];
        relative.Length = 10;
        relative.Data[0] = 2;
        relative.Data[1] = batch.NextState.Buttons;
        if (!absolute) {
            Put16(relative, 2, input.LastX);
            Put16(relative, 4, input.LastY);
        }
        if (input.ButtonFlags & (MOUSE_WHEEL | MOUSE_HWHEEL)) {
            const bool horizontal = (input.ButtonFlags & MOUSE_HWHEEL) != 0;
            int32_t& remainder = horizontal ? batch.NextState.HorizontalRemainder : batch.NextState.WheelRemainder;
            const int32_t delta = remainder + static_cast<int16_t>(input.ButtonData);
            Put16(relative, horizontal ? 8 : 6, delta / 120);
            remainder = delta % 120;
        }
    }
    return Result::Ok;
}

Result PrepareInput(uint32_t code, const void* data, size_t size,
                    const InputState& state, Batch& batch)
{
    batch = {};
    batch.NextState = state;
    if (code == IOCTL_KEYBOARD) {
        if (!data || size != sizeof(CQ_KEYBOARD_INPUT)) return Result::InvalidLength;
        CQ_KEYBOARD_INPUT input;
        memcpy(&input, data, sizeof(input));
        return Keyboard(input, batch);
    }
    if (code == IOCTL_MOUSE) {
        if (!data || size != sizeof(CQ_MOUSE_INPUT)) return Result::InvalidLength;
        CQ_MOUSE_INPUT input;
        memcpy(&input, data, sizeof(input));
        return Mouse(input, batch);
    }
    return Result::Unsupported;
}

Batch ReleaseAll()
{
    Batch batch = {};
    batch.Count = 2;
    batch.Reports[0] = KeyboardReport(batch.NextState);
    batch.Reports[1].Length = 10;
    batch.Reports[1].Data[0] = 2;
    return batch;
}
}
