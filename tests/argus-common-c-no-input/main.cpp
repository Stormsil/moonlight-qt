#include "Limelight.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

extern "C" {
void LiTestResetInputIsolationCounters(void);
void LiTestResetConnectionInputIsolationCounters(void);
void LiTestExerciseInputStages(void);
uint32_t LiTestGetControlInputSendCount(void);
uint32_t LiTestGetLegacyInputConnectCount(void);
uint32_t LiTestGetInputInitializeCount(void);
uint32_t LiTestGetInputStartCount(void);
}

namespace
{

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

int startRejectedAttempt(uint32_t flags)
{
    SERVER_INFORMATION server;
    LiInitializeServerInformation(&server);
    server.serverCodecModeSupport = 0;
    return LiStartConnection2(
        &server,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        0,
        nullptr,
        0,
        flags);
}

int startRejectedLegacyAttempt()
{
    SERVER_INFORMATION server;
    LiInitializeServerInformation(&server);
    server.serverCodecModeSupport = 0;
    return LiStartConnection(
        &server,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        0,
        nullptr,
        0);
}

std::vector<std::function<int()>> inputCalls()
{
    return {
        [] { return LiSendMouseMoveEvent(1, 1); },
        [] { return LiSendMousePositionEvent(1, 1, 2, 2); },
        [] { return LiSendMouseMoveAsMousePositionEvent(1, 1, 2, 2); },
        [] { return LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT); },
        [] { return LiSendKeyboardEvent(0, KEY_ACTION_DOWN, 0); },
        [] { return LiSendKeyboardEvent2(0, KEY_ACTION_DOWN, 0, 0); },
        [] { return LiSendUtf8TextEvent("x", 1); },
        [] { return LiSendControllerEvent(0, 0, 0, 0, 0, 0, 0); },
        [] { return LiSendMultiControllerEvent(0, 1, 0, 0, 0, 0, 0, 0, 0); },
        [] { return LiSendControllerArrivalEvent(0, 1, LI_CTYPE_XBOX, 0, 0); },
        [] { return LiSendControllerTouchEvent(0, LI_TOUCH_EVENT_DOWN, 1, 0.5f, 0.5f, 1.0f); },
        [] { return LiSendControllerTouchEvent2(0, LI_TOUCH_EVENT_DOWN, 0, 1, 0.5f, 0.5f, 1.0f); },
        [] { return LiSendControllerMotionEvent(0, LI_MOTION_TYPE_ACCEL, 0.0f, 0.0f, 0.0f); },
        [] { return LiSendControllerBatteryEvent(0, LI_BATTERY_STATE_FULL, 100); },
        [] { return LiSendScrollEvent(1); },
        [] { return LiSendHighResScrollEvent(1); },
        [] { return LiSendHScrollEvent(1); },
        [] { return LiSendHighResHScrollEvent(1); },
        [] { return LiSendTouchEvent(LI_TOUCH_EVENT_DOWN, 1, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0); },
        [] { return LiSendPenEvent(LI_TOUCH_EVENT_DOWN, LI_TOOL_TYPE_PEN, 0, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0, 0); },
    };
}

bool allInputCallsReturnDisabled()
{
    for (const auto& call : inputCalls()) {
        if (call() != LI_ERR_INPUT_DISABLED) {
            return false;
        }
    }
    return true;
}

}

int main()
{
    static_assert(LI_START_FLAG_DISABLE_INPUT_STREAM != 0);
    bool passed = true;
    LiTestResetInputIsolationCounters();
    LiTestResetConnectionInputIsolationCounters();

    passed &= expect(
        LiStartConnection2(
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            0,
            nullptr,
            0,
            UINT32_MAX) == LI_ERR_INVALID_ARGUMENT,
        "Unknown flags must fail before dereferencing arguments");
    passed &= expect(
        LiSendMouseMoveEvent(1, 1) == -2,
        "Unknown flags must not change the legacy input state");

    passed &= expect(
        startRejectedAttempt(LI_START_FLAG_DISABLE_INPUT_STREAM) != 0,
        "The bounded test attempt must stop before network startup");
    LiTestExerciseInputStages();
    passed &= expect(
        allInputCallsReturnDisabled(),
        "Every LiSend family must fail with LI_ERR_INPUT_DISABLED");

    std::atomic<bool> concurrentPassed = true;
    std::vector<std::thread> callers;
    for (int index = 0; index < 8; index++) {
        callers.emplace_back([&concurrentPassed] {
            if (!allInputCallsReturnDisabled()) {
                concurrentPassed.store(false);
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }
    passed &= expect(
        concurrentPassed.load(),
        "Concurrent LiSend calls must remain input-disabled");

    passed &= expect(
        LiStartConnection2(
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            0,
            nullptr,
            0,
            UINT32_MAX) == LI_ERR_INVALID_ARGUMENT
            && LiSendMouseMoveEvent(1, 1) == LI_ERR_INPUT_DISABLED,
        "Unknown flags must not downgrade an active disabled attempt");

    passed &= expect(
        startRejectedLegacyAttempt() != 0
            && LiSendMouseMoveEvent(1, 1) == -2,
        "The legacy ABI wrapper must restore flags=0 behavior");
    passed &= expect(
        startRejectedAttempt(LI_START_FLAG_DISABLE_INPUT_STREAM) != 0
            && allInputCallsReturnDisabled(),
        "Repeated legacy-to-disabled attempts must not leak state");

    LiStopConnection();
    passed &= expect(
        allInputCallsReturnDisabled(),
        "Disconnect must not reopen input for a disabled attempt");
    passed &= expect(
        LiTestGetControlInputSendCount() == 0,
        "Disabled attempts must send zero modern control input packets");
    passed &= expect(
        LiTestGetLegacyInputConnectCount() == 0,
        "Disabled attempts must make zero legacy TCP 35043 connections");
    passed &= expect(
        LiTestGetInputInitializeCount() == 0
            && LiTestGetInputStartCount() == 0,
        "Disabled attempts must not initialize or start input resources");

    return passed ? 0 : 1;
}
