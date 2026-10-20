#pragma once
#include "input_reports.h"

namespace cq {
// All operations require the caller's device lock. No allocations or waits.
struct ReportQueue {
    InputState State;
    Report Items[CQ_REPORT_QUEUE_CAPACITY];
    unsigned Head;
    unsigned Count;

    bool Push(const Batch& batch)
    {
        if (batch.Count > 2 || batch.Count > CQ_REPORT_QUEUE_CAPACITY - Count) return false;
        for (unsigned i = 0; i < batch.Count; ++i) {
            Items[(Head + Count) % CQ_REPORT_QUEUE_CAPACITY] = batch.Reports[i];
            ++Count;
        }
        State = batch.NextState;
        return true;
    }

    // Only call after observing Count > 0 while holding the same lock.
    Report Pop()
    {
        Report report = Items[Head];
        Head = (Head + 1) % CQ_REPORT_QUEUE_CAPACITY;
        --Count;
        return report;
    }

    void Clear()
    {
        Head = Count = 0;
        State = {};
    }

    void Release()
    {
        Clear();
        Push(ReleaseAll());
    }
};
}
