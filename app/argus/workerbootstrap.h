#pragma once

#include <QStringList>

namespace ArgusWorker
{

inline constexpr char StartupPipeEnvironmentVariable[] =
    "ARGUS_STREAM_STARTUP_PIPE";

enum ExitCode {
    ExitSuccess = 0,
    ExitInvalidArguments = 64,
    ExitStartupCapabilityUnavailable = 78,
    ExitHandshakeUnavailable = 79,
    ExitPairingRejected = 80,
    ExitStreamRejected = 81,
};

bool isRequested(int argc, char* argv[]);
bool isStreamInputIsolationSupported();

int run(int argc, char* argv[]);
int runStartup(const QStringList& arguments);

}
