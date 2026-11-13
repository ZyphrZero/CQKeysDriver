#pragma once
#include "../shared/cqkeys_protocol.h"

struct CqSessionState {
    uint64_t connectedEpoch;
    uint64_t epoch;
    uint64_t sessionId;
    uint32_t phase;
    bool terminal;

    bool CanBegin(uint64_t current) const {
        return !terminal && connectedEpoch == current && sessionId == 0 &&
            (phase == CQKEYS_SESSION_CONNECTED || phase == CQKEYS_SESSION_STARTING ||
             phase == CQKEYS_SESSION_CLEANUP_FAILED);
    }
    void Activate(uint64_t current, uint64_t identity) {
        epoch = current;
        sessionId = identity;
        phase = CQKEYS_SESSION_ACTIVE;
    }
    bool Matches(const CqRequestHeader& header) const {
        return epoch == header.deviceEpoch && sessionId != 0 && sessionId == header.sessionId;
    }
    bool CanSubmit(uint64_t current) const {
        return phase == CQKEYS_SESSION_ACTIVE && epoch == current;
    }
    void Stop() {
        terminal = true;
        if (phase != CQKEYS_SESSION_INVALIDATED) phase = CQKEYS_SESSION_STOPPING;
    }
    void FinishStop(bool success) {
        if (phase != CQKEYS_SESSION_INVALIDATED && phase != CQKEYS_SESSION_CLOSED)
            phase = success ? CQKEYS_SESSION_STOPPED : CQKEYS_SESSION_CLEANUP_FAILED;
    }
    void Invalidate() { terminal = true; phase = CQKEYS_SESSION_INVALIDATED; }
};

struct CqInputState {
    CqKeyboardReport keyboard;
    uint8_t buttons;
    uint16_t consumer;
    uint32_t pendingCleanup;
    uint64_t lastAcceptedSequence;
    uint64_t lastSubmittedSequence;
    int32_t inputStatus;
    int32_t cleanupStatus;
    int32_t firstCleanupStatus;

    void RequireNeutral() { pendingCleanup |= CQKEYS_CLEANUP_ALL; }
    void Commit(const CqReportRequest& request) {
        switch (request.report[0]) {
        case CQKEYS_KEYBOARD_ID:
            memcpy(&keyboard, request.report, sizeof(keyboard));
            {
                bool held = keyboard.modifiers != 0;
                for (auto key : keyboard.keys) held = held || key != 0;
                if (held) pendingCleanup |= CQKEYS_CLEANUP_KEYBOARD;
                else pendingCleanup &= ~CQKEYS_CLEANUP_KEYBOARD;
            }
            break;
        case CQKEYS_MOUSE_ID: {
            CqMouseReport mouse;
            memcpy(&mouse, request.report, sizeof(mouse));
            buttons = mouse.buttons;
            if (buttons) pendingCleanup |= CQKEYS_CLEANUP_MOUSE;
            else pendingCleanup &= ~CQKEYS_CLEANUP_MOUSE;
            break;
        }
        case CQKEYS_CONSUMER_ID: {
            CqConsumerReport report;
            memcpy(&report, request.report, sizeof(report));
            consumer = report.usage;
            if (consumer) pendingCleanup |= CQKEYS_CLEANUP_CONSUMER;
            else pendingCleanup &= ~CQKEYS_CLEANUP_CONSUMER;
            break;
        }
        }
        lastSubmittedSequence = request.sequence;
        inputStatus = 0;
    }
    template<typename Submit> int32_t Neutralize(Submit submit) {
        int32_t first = 0;
        auto record = [&](uint32_t mask, int32_t status) {
            if (status >= 0) pendingCleanup &= ~mask;
            else {
                if (first >= 0) first = status;
                if (firstCleanupStatus >= 0) firstCleanupStatus = status;
            }
        };
        if (pendingCleanup & CQKEYS_CLEANUP_KEYBOARD) {
            CqKeyboardReport neutral{CQKEYS_KEYBOARD_ID};
            auto status = submit(&neutral, sizeof(neutral));
            if (status >= 0) keyboard = neutral;
            record(CQKEYS_CLEANUP_KEYBOARD, status);
        }
        if (pendingCleanup & CQKEYS_CLEANUP_MOUSE) {
            CqMouseReport neutral{CQKEYS_MOUSE_ID};
            auto status = submit(&neutral, sizeof(neutral));
            if (status >= 0) buttons = 0;
            record(CQKEYS_CLEANUP_MOUSE, status);
        }
        if (pendingCleanup & CQKEYS_CLEANUP_CONSUMER) {
            CqConsumerReport neutral{CQKEYS_CONSUMER_ID};
            auto status = submit(&neutral, sizeof(neutral));
            if (status >= 0) consumer = 0;
            record(CQKEYS_CLEANUP_CONSUMER, status);
        }
        cleanupStatus = first;
        return first;
    }
};
