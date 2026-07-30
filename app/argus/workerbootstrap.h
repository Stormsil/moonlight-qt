#pragma once

#include <QStringList>

namespace ArgusWorker
{

inline constexpr char StartupPipeEnvironmentVariable[] =
    "ARGUS_STREAM_STARTUP_PIPE";

enum ExitCode {
    ExitSuccess = 0,
    ExitInvalidArguments = 64,
    ExitHandshakeProtocolPending = 70,
    ExitStartupCapabilityUnavailable = 78,
    ExitHandshakeUnavailable = 79,
};

bool isRequested(int argc, char* argv[]);

int run(int argc, char* argv[]);
int runStartup(const QStringList& arguments);

}
