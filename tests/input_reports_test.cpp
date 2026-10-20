#include "../cqkeys/input_reports.h"
#include "../cqkeys/report_queue.h"
#include "../cqkeys/hid_descriptor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void CheckDescriptor()
{
    unsigned bits[4] = {};
    unsigned reportId = 0, reportSize = 0, reportCount = 0, depth = 0;
    for (size_t offset = 0; offset < sizeof(CqReportDescriptor);) {
        const unsigned prefix = CqReportDescriptor[offset++];
        assert(prefix != 0xfe); // This descriptor uses only short items.
        const unsigned size = (prefix & 3) == 3 ? 4 : prefix & 3;
        assert(offset + size <= sizeof(CqReportDescriptor));
        uint32_t value = 0;
        for (unsigned i = 0; i < size; ++i)
            value |= static_cast<uint32_t>(CqReportDescriptor[offset++]) << (i * 8);
        const unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1 && tag == 7) reportSize = value;
        if (type == 1 && tag == 8) { reportId = value; assert(reportId > 0 && reportId < 4); }
        if (type == 1 && tag == 9) reportCount = value;
        if (type == 0 && tag == 8) bits[reportId] += reportSize * reportCount;
        if (type == 0 && tag == 10) ++depth;
        if (type == 0 && tag == 12) { assert(depth > 0); --depth; }
    }
    assert(depth == 0);
    assert(bits[1] == 64 && bits[2] == 72 && bits[3] == 40);
}

int main()
{
    CheckDescriptor();
    cq::InputState state = {};
    cq::Batch batch = {};
    unsigned char shortInput = 0;
    // Short requests must be rejected before any structure access.
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &shortInput, 1, state, batch) == cq::Result::InvalidLength);
    assert(cq::PrepareInput(IOCTL_MOUSE, &shortInput, 1, state, batch) == cq::Result::InvalidLength);
    assert(cq::PrepareInput(IOCTL_KEYBOARD, nullptr, 0, state, batch) == cq::Result::InvalidLength);

    CQ_KEYBOARD_INPUT key = {};
    key.MakeCode = 0x1e; // A
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Ok);
    assert(batch.Count == 1 && batch.Reports[0].Length == 9);
    assert(batch.Reports[0].Data[0] == 1 && batch.Reports[0].Data[3] == 4);
    assert(state.Keys[0] == 0); // Caller only commits after queue admission.
    state = batch.NextState;
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Ok);
    assert(batch.NextState.Keys[1] == 0); // Repeat does not consume another slot.
    key.Flags = KEY_BREAK;
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Ok);
    assert(batch.NextState.Keys[0] == 0);

    state = {};
    key.MakeCode = 0x1d;
    key.Flags = KEY_E0; // Right Ctrl, not Left Ctrl.
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Ok);
    assert(batch.Reports[0].Data[1] == 0x10);
    key.MakeCode = 0x48; // Up, not keypad 8.
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Ok);
    assert(batch.Reports[0].Data[3] == 0x52);
    key.Flags = KEY_E0 | KEY_E1;
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::InvalidInput);
    key = {};
    key.MakeCode = 0x7f;
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Unsupported);
    key.MakeCode = 0x1e;
    key.ExtraInformation = 1;
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Unsupported);
    key.ExtraInformation = 0;
    for (unsigned i = 0; i != 6; ++i) state.Keys[i] = static_cast<uint8_t>(5 + i);
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Full);
    key.Flags = KEY_BREAK;
    assert(cq::PrepareInput(IOCTL_KEYBOARD, &key, sizeof(key), state, batch) == cq::Result::Ok);

    state = {};
    CQ_MOUSE_INPUT mouse = {};
    mouse.LastX = -32768;
    mouse.LastY = 32767;
    mouse.ButtonFlags = MOUSE_LEFT_BUTTON_DOWN;
    assert(cq::PrepareInput(IOCTL_MOUSE, &mouse, sizeof(mouse), state, batch) == cq::Result::Ok);
    assert(batch.Reports[0].Length == 10 && batch.Reports[0].Data[0] == 2);
    assert(batch.Reports[0].Data[1] == 1 && batch.Reports[0].Data[3] == 0x80);
    assert(batch.Reports[0].Data[4] == 0xff && batch.Reports[0].Data[5] == 0x7f);
    state = batch.NextState;
    mouse.LastX = 32768;
    assert(cq::PrepareInput(IOCTL_MOUSE, &mouse, sizeof(mouse), state, batch) == cq::Result::InvalidInput);
    mouse = {};
    mouse.Flags = MOUSE_MOVE_ABSOLUTE;
    mouse.LastX = 65535;
    mouse.LastY = 123;
    mouse.ButtonFlags = MOUSE_LEFT_BUTTON_UP;
    assert(cq::PrepareInput(IOCTL_MOUSE, &mouse, sizeof(mouse), state, batch) == cq::Result::Ok);
    assert(batch.Count == 2 && batch.NextState.Buttons == 0);
    assert(batch.Reports[0].Data[0] == 3 && batch.Reports[0].Data[2] == 0xff);
    assert(batch.Reports[1].Data[0] == 2 && batch.Reports[1].Data[1] == 0);
    mouse.LastX = -1;
    assert(cq::PrepareInput(IOCTL_MOUSE, &mouse, sizeof(mouse), state, batch) == cq::Result::InvalidInput);
    mouse.Flags = MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP;
    assert(cq::PrepareInput(IOCTL_MOUSE, &mouse, sizeof(mouse), state, batch) == cq::Result::Unsupported);

    state = {};
    mouse = {};
    mouse.ButtonFlags = MOUSE_WHEEL;
    mouse.ButtonData = 60;
    assert(cq::PrepareInput(IOCTL_MOUSE, &mouse, sizeof(mouse), state, batch) == cq::Result::Ok);
    assert(batch.Reports[0].Data[6] == 0 && batch.NextState.WheelRemainder == 60);
    state = batch.NextState;
    assert(cq::PrepareInput(IOCTL_MOUSE, &mouse, sizeof(mouse), state, batch) == cq::Result::Ok);
    assert(batch.Reports[0].Data[6] == 1 && batch.NextState.WheelRemainder == 0);
    mouse.ButtonFlags = MOUSE_LEFT_BUTTON_DOWN | MOUSE_LEFT_BUTTON_UP;
    assert(cq::PrepareInput(IOCTL_MOUSE, &mouse, sizeof(mouse), state, batch) == cq::Result::InvalidInput);
    assert(cq::PrepareInput(0, &mouse, sizeof(mouse), state, batch) == cq::Result::Unsupported);

    batch = cq::ReleaseAll();
    assert(batch.Count == 2);
    assert(batch.Reports[0].Data[0] == 1 && batch.Reports[1].Data[0] == 2);
    for (unsigned i = 1; i < 9; ++i) assert(batch.Reports[0].Data[i] == 0);
    for (unsigned i = 1; i < 10; ++i) assert(batch.Reports[1].Data[i] == 0);
    cq::ReportQueue queue = {};
    cq::Batch one = {};
    one.Count = 1;
    one.NextState.Buttons = 1;
    for (unsigned i = 0; i < CQ_REPORT_QUEUE_CAPACITY; ++i) {
        one.Reports[0].Data[0] = static_cast<uint8_t>(i);
        assert(queue.Push(one));
    }
    one.NextState.Buttons = 2;
    assert(!queue.Push(one) && queue.Count == CQ_REPORT_QUEUE_CAPACITY);
    assert(queue.State.Buttons == 1); // A rejected request cannot change state.
    assert(queue.Pop().Data[0] == 0);
    assert(!queue.Push(batch)); // A two-report batch must fit atomically.
    assert(queue.Count == CQ_REPORT_QUEUE_CAPACITY - 1);
    assert(queue.Push(one)); // Wrap around without overwriting queued data.
    for (unsigned i = 1; i < CQ_REPORT_QUEUE_CAPACITY; ++i)
        assert(queue.Pop().Data[0] == i);
    assert(queue.Pop().Data[0] == CQ_REPORT_QUEUE_CAPACITY - 1);
    assert(queue.Count == 0);
    queue.Release();
    assert(queue.Count == 2 && queue.State.Buttons == 0);
    assert(queue.Pop().Data[0] == 1);
    assert(queue.Pop().Data[0] == 2);
    puts("input report regression tests passed");
}
