#define CQ_INPUT_TASK_TEST
#include "native_lifecycle_test.cpp"

CqInputTaskResult resultFor(uint64_t taskId) {
    CqInputTaskResult result{};
    result.size = sizeof(result);
    result.version = CQ_INPUT_TASK_ABI;
    check(QueryInputTask(taskId, &result), "query fixed result");
    return result;
}

int main() {
    uint64_t taskId = 0, otherId = 0;
    CqInputAction tap[]{{CQ_INPUT_KEY_DOWN, 'A', 0, 0}, {CQ_INPUT_KEY_UP, 'A', 0, 0}};
    CqInputAction delay{CQ_INPUT_WAIT, 0, 0, 60000};
    fake::reset(); check(SetHandle(), "task session");
    check(!SubmitInputTask(nullptr, 1, 1, 0, &taskId) && GetLastError() == ERROR_INVALID_PARAMETER, "null actions");
    check(!SubmitInputTask(tap, 0, 1, 0, &taskId), "empty task");
    check(!SubmitInputTask(tap, CQ_INPUT_TASK_MAX_ACTIONS + 1, 1, 0, &taskId), "bounded action count");
    check(!SubmitInputTask(tap, 2, 0, 0, &taskId), "finite nonzero repeats");
    check(!SubmitInputTask(tap, 2, 1, 60001, &taskId), "bounded delay");
    CqInputAction invalid{CQ_INPUT_BUTTONS, 32, 0, 0};
    check(!SubmitInputTask(&invalid, 1, 1, 0, &taskId), "invalid action rejected before worker");
    KeyDown('B');
    check(!SubmitInputTask(tap, 2, 1, 0, &taskId) && GetLastError() == ERROR_BUSY, "legacy held key prevents task");
    KeyUp('B');
    check(SubmitInputTask(tap, 2, 3, 0, &taskId), "finite repeats");
    check(WaitInputTask(taskId, 3000), "finite task finishes");
    auto result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_COMPLETED && result.completedActions == 6 && result.completedRepeats == 3,
        "repeat counts reflect completed work");
    check(result.lastAcceptedSequence == result.lastSubmittedSequence && !result.cleanupError, "confirmed sequences");
    check(!SubmitInputTask(tap, 2, 1, 0, &otherId) && GetLastError() == ERROR_BUSY, "one retained task slot");
    check(ReleaseInputTask(taskId), "release joins worker");
    check(!WaitInputTask(taskId, 0) && GetLastError() == ERROR_NOT_FOUND, "released task identifier rejected");

    CqInputAction heldInputs[]{{CQ_INPUT_KEY_DOWN, 'A', 0, 0}, {CQ_INPUT_CONSUMER_DOWN, 0xe9, 0, 0},
        {CQ_INPUT_BUTTONS, 17, 0, 0}, {CQ_INPUT_MOVE_RELATIVE, 70000, -70000, 0},
        {CQ_INPUT_MOVE_ABSOLUTE, 1919, 1079, 0}, {CQ_INPUT_WHEEL, -2, 0, 0}};
    check(SubmitInputTask(heldInputs, 6, 1, 0, &taskId) && WaitInputTask(taskId, 3000), "mixed finite task");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_COMPLETED && result.completedActions == 6 && !result.resultFlags,
        "normal task completes all action classes");
    check(result.sessionPhase == CQKEYS_SESSION_ACTIVE && result.pendingCleanup == 0,
        "neutral task reports clear cleanup mask without stopping session");
    check(fake::state.keyboard.keys[0] == 0 && !fake::state.buttons && !fake::state.consumer, "normal task releases all held input");
    check(ReleaseInputTask(taskId), "release mixed task");

    check(SubmitInputTask(&delay, 1, 1, 0, &taskId), "cancelable interval");
    check(!WaitInputTask(taskId, 0) && GetLastError() == WAIT_TIMEOUT, "timeout is distinct from completion");
    KeyDown('C'); check(GetLastError() == ERROR_BUSY, "legacy primitive busy during task");
    check(!ReleaseInputTask(taskId) && GetLastError() == ERROR_BUSY, "running task cannot release");
    check(CancelInputTask(taskId) && WaitInputTask(taskId, 3000), "cancel wakes long interval");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_CANCELLED && !result.cleanupError, "cancel terminal result");
    check(result.sessionPhase == CQKEYS_SESSION_ACTIVE && fake::stops == 0, "task cancel preserves active session");
    check(ReleaseInputTask(taskId), "release cancellation");
    check(SubmitInputTask(tap, 2, 1, 0, &taskId) && WaitInputTask(taskId, 3000), "next task works after cancellation");
    check(ReleaseInputTask(taskId) && CleanupInputRuntime(), "explicit unload-safe cleanup");

    fake::reset(); check(SetHandle(), "pending cancellation session");
    fake::blockReport = fake::deferCompletion = true;
    check(SubmitInputTask(tap, 2, 1, 0, &taskId) && waitPending(), "task report pending");
    check(CancelInputTask(taskId), "cancel outstanding task report");
    check(!WaitInputTask(taskId, 0), "cancellation request is not actual completion");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex);
        check(fake::cancels >= 1 && fake::stops == 0 && fake::closes == 0, "no whole-session stop on task cancellation");
        fake::blockReport = false;
        fake::deferCompletion = false;
    }
    fake::ioCondition.notify_all();
    check(WaitInputTask(taskId, 3000), "drain then release task input");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_CANCELLED && result.sessionPhase == CQKEYS_SESSION_ACTIVE, "pending cancel leaves active session");
    check(ReleaseInputTask(taskId) && CleanupInputRuntime(), "pending cancel cleanup");

    fake::reset(); check(SetHandle(), "cancel held input session");
    CqInputAction heldDelay{CQ_INPUT_KEY_DOWN, 'A', 0, 60000};
    check(SubmitInputTask(&heldDelay, 1, 1, 0, &taskId), "hold key then interval");
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return !fake::submitted.empty(); }), "held key submitted");
    }
    check(CancelInputTask(taskId) && WaitInputTask(taskId, 3000), "cancel held key task");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_CANCELLED && (result.resultFlags & CQ_INPUT_RESULT_PARTIAL) &&
        (result.resultFlags & CQ_INPUT_RESULT_CANCELLED) && !fake::state.keyboard.keys[0], "cancel releases confirmed held key");
    check(CleanupInputRuntime(), "held input cleanup");

    fake::reset(); check(SetHandle(), "early cancellation session");
    fake::holdIssue = fake::blockReport = fake::blockOnce = true;
    check(SubmitInputTask(tap, 2, 1, 0, &taskId), "early cancellation task");
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::issueEntered; }), "task inside API before issuance");
    }
    check(CancelInputTask(taskId) && fake::cancels == 0, "early CancelIoEx not-found is not completion");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex); fake::releaseIssue = true;
    }
    fake::ioCondition.notify_all();
    check(WaitInputTask(taskId, 3000), "post-issuance cancellation retry drains real completion");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_CANCELLED && fake::cancels == 1 && fake::stops == 0,
        "early cancellation race preserves active session");
    check(CleanupInputRuntime(), "early cancellation cleanup");

    fake::reset(); check(SetHandle(), "neutral cancellation session");
    check(SubmitInputTask(&heldDelay, 1, 1, 0, &taskId), "neutral cancellation task");
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return !fake::submitted.empty(); }), "key held before neutral cleanup");
        fake::blockReport = fake::blockOnce = true;
    }
    check(CancelInputTask(taskId) && waitPending(), "neutral cleanup is pending");
    check(CancelInputTask(taskId) && fake::cancels == 0, "repeated task cancellation must not cancel neutral reports");
    check(!WaitInputTask(taskId, 0) && GetLastError() == WAIT_TIMEOUT, "pending neutral is not task completion");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex);
        check(fake::reports == 2, "wait timeout does not replay reports");
        fake::completePending(ERROR_SUCCESS);
    }
    check(WaitInputTask(taskId, 3000), "neutral report completes before terminal task result");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_CANCELLED && !result.cleanupError && !result.pendingCleanup && fake::reports == 4,
        "only one keyboard release plus mouse and consumer neutral reports");
    check(CleanupInputRuntime(), "neutral cancellation cleanup");

    fake::reset(); check(SetHandle(), "task STOP ownership session");
    fake::blockReport = fake::deferCompletion = fake::asyncStop = true;
    check(SubmitInputTask(tap, 2, 1, 0, &taskId) && waitPending(), "task report pending before STOP");
    std::atomic<bool> stopReturned{false};
    std::thread stopping([&] { check(StopInputProcessing(), "task STOP completion"); stopReturned = true; });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::pendingControl != nullptr; }), "task STOP pending");
        check(fake::cancels >= 1 && !stopReturned && !fake::closes, "STOP cannot release resources while report completion is withheld");
        fake::deferCompletion = false;
    }
    fake::ioCondition.notify_all();
    check(!WaitInputTask(taskId, 20) && GetLastError() == WAIT_TIMEOUT, "drained report does not steal pending STOP completion");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex);
        fake::releaseControl = true;
    }
    fake::ioCondition.notify_all(); stopping.join();
    check(WaitInputTask(taskId, 0), "STOP joins task after real control completion");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_CANCELLED && !result.cleanupError && result.sessionPhase == CQKEYS_SESSION_STOPPED &&
        !g_taskThread && !g_taskActive && fake::submitted.empty(), "single terminal task result after STOP without replay");
    check(CleanupInputRuntime(), "task STOP ownership cleanup");

    fake::reset(); check(SetHandle(), "partial task session"); fake::failReportAt = 2;
    CqInputAction movement{CQ_INPUT_MOVE_RELATIVE, 70000, 20, 0};
    check(SubmitInputTask(&movement, 1, 2, 0, &taskId) && WaitInputTask(taskId, 3000), "partial movement failure completes task");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_FAILED && result.error == ERROR_WRITE_FAULT && !result.completedActions,
        "partial action is not counted as completed");
    check(fake::reports == 2 && fake::submitted.size() == 1 && result.lastAcceptedSequence == 2 && result.lastSubmittedSequence == 1,
        "no replay; accepted and submitted are distinct");
    check((result.resultFlags & CQ_INPUT_RESULT_PARTIAL) && (result.resultFlags & CQ_INPUT_RESULT_UNCERTAIN),
        "failed action exposes partial side effects even with zero completed actions");
    KeyDown('B'); check(GetLastError() == ERROR_NOT_READY, "failed task gates legacy input");
    check(ReleaseInputTask(taskId), "failed result can release");
    check(!SubmitInputTask(tap, 2, 1, 0, &taskId) && GetLastError() == ERROR_NOT_READY, "failed task prevents later input");
    check(CleanupInputRuntime(), "partial task cleanup");

    fake::reset(); check(SetHandle(), "cleanup failure session"); fake::failReportAt = 2; fake::stopError = ERROR_WRITE_FAULT;
    CqInputAction held{CQ_INPUT_KEY_DOWN, 'A', 0, 0};
    check(SubmitInputTask(&held, 1, 1, 0, &taskId) && WaitInputTask(taskId, 3000), "task release fails");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_FAILED && result.cleanupError == ERROR_WRITE_FAULT && result.pendingCleanup,
        "cleanup failure visible in result");
    check(!CleanupInputRuntime() && fake::lease && !fake::closes, "failed cleanup retains handle and lease");
    fake::stopError = 0; check(CleanupInputRuntime() && !fake::lease, "retry cleanup drains and releases resources");
    fake::reset(); check(SetHandle(), "final diagnosis failure session"); fake::failQueryAfterReports = 5;
    check(SubmitInputTask(tap, 2, 1, 0, &taskId) && WaitInputTask(taskId, 3000), "final query failure reaches terminal state");
    result = resultFor(taskId);
    check(result.state == CQ_INPUT_TASK_FAILED && result.error == ERROR_GEN_FAILURE &&
        (result.resultFlags & CQ_INPUT_RESULT_UNCERTAIN), "missing final diagnosis cannot masquerade as successful task");
    check(CleanupInputRuntime(), "query failure still permits successful explicit STOP cleanup");
    fake::reset(); check(SetHandle(), "thread close retry session");
    check(SubmitInputTask(tap, 2, 1, 0, &taskId) && WaitInputTask(taskId, 3000), "thread close retry task completes");
    fake::failThreadClose = true;
    check(!ReleaseInputTask(taskId) && GetLastError() == ERROR_INVALID_HANDLE && g_taskThread && g_taskResult.taskId == taskId,
        "failed thread close retains handle and result for retry");
    fake::failThreadClose = false;
    check(ReleaseInputTask(taskId) && CleanupInputRuntime(), "thread close retry releases actual handle");
    DWORD handlesBefore = 0, handlesAfter = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore), "measure task handle baseline");
    for (int cycle = 0; cycle < 100; ++cycle) {
        fake::reset(); check(SetHandle(), "repeated task open");
        check(SubmitInputTask(tap, 2, 1, 0, &taskId) && WaitInputTask(taskId, 3000), "repeated task execution");
        check(CleanupInputRuntime() && !g_taskThread && !g_taskActive && !g_taskResult.taskId, "no retained thread or task at unload boundary");
        std::lock_guard<std::mutex> handles(fake::handleMutex);
        check(fake::liveThreads.empty() && fake::liveEvents.empty(), "all DLL-created thread and IO event handles are closed");
    }
    check(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter), "measure task handles after cleanup");
    std::cout << "Process handles (including CRT internals): " << handlesBefore << " -> " << handlesAfter << '\n';
    if (failures) return 1;
    std::cout << "Native input task tests passed (actual DLL implementation; fake Win32 device boundary)\n";
    return 0;
}
