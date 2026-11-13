#include <windows.h>
#include <winioctl.h>
#include <cstring>
#include <iostream>
#include <vector>
#include "../cqkeys/input_state.h"
#include "../shared/cqkeys_reports.h"

namespace {
    int failures = 0;
    void Check(bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    }
    template<typename Report> CqReportRequest Request(const Report& report, uint64_t sequence = 1) {
        CqReportRequest request{};
        request.header = CqHeader(sizeof(request), 7, 9);
        request.sequence = sequence;
        request.reportLength = sizeof(report);
        std::memcpy(request.report, &report, sizeof(report));
        return request;
    }
    void TestProtocol() {
        Check(CQKEYS_MAX_REPORT_LENGTH == sizeof(CqKeyboardReport) && CQKEYS_MAX_REPORT_LENGTH == 9,
            "capabilities advertise actual HID maximum rather than padded report slot");
        auto request = Request(CqKeyboardReport{CQKEYS_KEYBOARD_ID});
        Check(CqValidReport(request), "neutral keyboard is valid");
        request.header.minor = 1;
        Check(!CqValidReport(request), "unknown minor is rejected");
        request.header.minor = 0; request.sequence = 0;
        Check(!CqValidReport(request), "zero sequence is rejected");
        request.sequence = 1; request.reserved = 1;
        Check(!CqValidReport(request), "nonzero reserved field is rejected");
        request.reserved = 0; request.report[15] = 1;
        Check(!CqValidReport(request), "nonzero report tail is rejected");
        request = Request(CqKeyboardReport{CQKEYS_KEYBOARD_ID, 0, 0, {4, 4}});
        Check(!CqValidReport(request), "duplicate keys are rejected");
        request = Request(CqKeyboardReport{CQKEYS_KEYBOARD_ID, 0, 0, {0x74}});
        Check(!CqValidReport(request), "keyboard upper boundary is enforced");
        request = Request(CqMouseReport{CQKEYS_MOUSE_ID, 31, -32767, 32767, -127});
        Check(CqValidReport(request), "mouse boundaries are valid");
        request.report[1] = 32;
        Check(!CqValidReport(request), "mouse button padding is rejected");
        request = Request(CqMouseReport{CQKEYS_MOUSE_ID, 0, -32768, 0, 0});
        Check(!CqValidReport(request), "relative axis lower boundary is enforced");
        request = Request(CqAbsoluteReport{CQKEYS_ABSOLUTE_ID, 32768, 0});
        Check(!CqValidReport(request), "absolute axis boundary is enforced");
        request = Request(CqConsumerReport{CQKEYS_CONSUMER_ID, 0x400});
        Check(!CqValidReport(request), "consumer boundary is enforced");
        request.report[0] = 0xff;
        Check(!CqValidReport(request), "unknown report is rejected");
    }
    void TestState() {
        CqInputState state{};
        std::vector<uint8_t> order;
        auto submit = [&](void* data, uint32_t) -> int32_t {
            auto report = static_cast<uint8_t*>(data);
            order.push_back(report[0]);
            return report[0] == CQKEYS_MOUSE_ID ? static_cast<int32_t>(0xc0000185u) : 0;
        };
        state.RequireNeutral();
        state.Commit(Request(CqKeyboardReport{CQKEYS_KEYBOARD_ID, 0x10, 0, {4}}));
        state.Commit(Request(CqMouseReport{CQKEYS_MOUSE_ID, 17, 10, -5, 1}, 2));
        state.Commit(Request(CqConsumerReport{CQKEYS_CONSUMER_ID, 0xcd}, 3));
        Check(state.lastSubmittedSequence == 3 && state.keyboard.keys[0] == 4,
            "production state records successful report and sequence");
        Check(state.Neutralize(submit) < 0, "partial neutralization fails");
        Check(order == std::vector<uint8_t>({1, 2, 4}), "release order is keyboard mouse consumer");
        Check(state.pendingCleanup == CQKEYS_CLEANUP_MOUSE && state.buttons == 17,
            "only failed state remains pending");
        Check(state.keyboard.keys[0] == 0 && state.consumer == 0,
            "successful releases commit independently");
        auto firstError = state.firstCleanupStatus;
        order.clear();
        Check(state.Neutralize([&](void* report, uint32_t length) -> int32_t {
            order.push_back(*static_cast<uint8_t*>(report));
            Check(length == sizeof(CqMouseReport), "retry uses exact mouse report");
            const auto& mouse = *static_cast<CqMouseReport*>(report);
            Check(mouse.x == 0 && mouse.y == 0 && mouse.wheel == 0 && mouse.buttons == 0,
                "cleanup never replays relative movement");
            return 0;
        }) == 0, "failed cleanup is retryable");
        Check(order == std::vector<uint8_t>({2}) && state.pendingCleanup == 0,
            "retry does not repeat successful releases");
        Check(firstError < 0 && state.firstCleanupStatus == firstError && state.cleanupStatus == 0,
            "successful retry retains original error");
        Check(state.buttons == 0 && state.lastSubmittedSequence == 3,
            "cleanup releases state without manufacturing input receipts");
        state.Commit(Request(CqMouseReport{CQKEYS_MOUSE_ID, 0, 10, -5, 1}, 4));
        Check(state.pendingCleanup == 0, "relative movement and wheel do not create held state");
        state.Commit(Request(CqKeyboardReport{CQKEYS_KEYBOARD_ID, 1}, 5));
        Check(state.pendingCleanup == CQKEYS_CLEANUP_KEYBOARD, "modifier-only input is held state");
        state.Commit(Request(CqKeyboardReport{CQKEYS_KEYBOARD_ID}, 6));
        Check(state.pendingCleanup == 0, "successful neutral input clears held keyboard state");
        state.Commit(Request(CqConsumerReport{CQKEYS_CONSUMER_ID, 0xcd}, 7));
        state.Commit(Request(CqConsumerReport{CQKEYS_CONSUMER_ID, 0}, 8));
        Check(state.pendingCleanup == 0, "task release reports leave a clean active session");
    }
    void TestSession() {
        CqSessionState session{};
        session.connectedEpoch = 7;
        Check(session.CanBegin(7), "fresh file can begin");
        session.Activate(7, 9);
        auto request = Request(CqKeyboardReport{CQKEYS_KEYBOARD_ID});
        Check(session.Matches(request.header) && session.CanSubmit(7), "active binding permits input");
        request.header.sessionId = 10;
        Check(!session.Matches(request.header), "foreign session is rejected");
        session.Stop();
        Check(!session.CanSubmit(7) && !session.CanBegin(7), "stopped file cannot reactivate");
        session.phase = CQKEYS_SESSION_ACTIVE;
        session.Invalidate();
        Check(!session.CanBegin(8) && !session.CanSubmit(8), "power invalidation cannot refresh binding");
        Check(session.epoch == 7 && session.sessionId == 9, "invalidation preserves cleanup identity");
        CqSessionState initial{};
        initial.connectedEpoch = 7;
        initial.Stop();
        initial.phase = CQKEYS_SESSION_CLEANUP_FAILED;
        Check(!initial.CanBegin(7), "failed stop before activation is also terminal");
        CqSessionState retry{};
        retry.connectedEpoch = 7;
        retry.phase = CQKEYS_SESSION_CLEANUP_FAILED;
        Check(retry.CanBegin(7), "failed initial baseline can be retried before stop");
        retry.Invalidate();
        retry.Stop();
        retry.FinishStop(true);
        Check(retry.phase == CQKEYS_SESSION_INVALIDATED, "successful stop preserves power invalidation");
        retry.FinishStop(false);
        Check(retry.phase == CQKEYS_SESSION_INVALIDATED, "failed cleanup preserves power invalidation");
        retry.phase = CQKEYS_SESSION_CLEANUP_FAILED;
        Check(!retry.CanBegin(7), "cleanup failure cannot undo power invalidation");
    }
    void TestCleanupFailures() {
        for (uint32_t failureMask = 0; failureMask <= CQKEYS_CLEANUP_ALL; ++failureMask) {
            CqInputState state{};
            state.RequireNeutral();
            uint32_t calls = 0;
            auto status = state.Neutralize([&](void* data, uint32_t) -> int32_t {
                ++calls;
                auto reportId = *static_cast<uint8_t*>(data);
                uint32_t mask = reportId == CQKEYS_CONSUMER_ID ?
                    static_cast<uint32_t>(CQKEYS_CLEANUP_CONSUMER) : reportId;
                return failureMask & mask ? static_cast<int32_t>(0xc0000185u) : 0;
            });
            Check(calls == 3 && state.pendingCleanup == failureMask, "all cleanup failure subsets retain exact mask");
            Check((status < 0) == (failureMask != 0), "cleanup never reports partial success as complete");
            uint32_t expectedCalls = 0;
            for (uint32_t mask = 1; mask <= CQKEYS_CLEANUP_CONSUMER; mask <<= 1)
                if (failureMask & mask) ++expectedCalls;
            calls = 0;
            Check(state.Neutralize([&](void*, uint32_t) -> int32_t { ++calls; return 0; }) == 0,
                "all failed subsets can be retried");
            Check(calls == expectedCalls && state.pendingCleanup == 0, "retry touches only failed subsets");
        }
    }
    void TestDescriptor() {
        uint32_t bits[5]{}, reportId = 0, reportSize = 0, reportCount = 0, depth = 0, collections = 0;
        for (size_t offset = 0; offset < sizeof(CqReportDescriptor);) {
            uint32_t prefix = CqReportDescriptor[offset++], size = prefix & 3, value = 0;
            if (size == 3) size = 4;
            if (offset + size > sizeof(CqReportDescriptor)) { Check(false, "descriptor bound"); return; }
            for (uint32_t byte = 0; byte < size; ++byte) value |= CqReportDescriptor[offset++] << (8 * byte);
            switch (prefix & 0xfc) {
            case 0x84: reportId = value; break;
            case 0x74: reportSize = value; break;
            case 0x94: reportCount = value; break;
            case 0x80:
                Check(reportId > 0 && reportId < 5, "descriptor report id");
                if (reportId < 5) bits[reportId] += reportSize * reportCount;
                break;
            case 0xa0: if (depth++ == 0) ++collections; break;
            case 0xc0: Check(depth != 0, "balanced collection"); --depth; break;
            }
        }
        Check(depth == 0 && collections == 4, "four HID collections");
        Check(bits[1] == 64 && bits[2] == 48 && bits[3] == 32 && bits[4] == 16,
            "fixed HID descriptor matches wire reports");
    }
}
int main() {
    TestProtocol(); TestState(); TestSession(); TestCleanupFailures(); TestDescriptor();
    if (failures) return 1;
    std::cout << "Protocol and production input state tests passed (no driver loaded)\n";
    return 0;
}
