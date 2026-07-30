#include "workerbootstrap.h"
#include "startupchannel.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <cstring>

namespace
{

constexpr int MaximumStartupPipeNameCharacters = 240;

bool parseArguments(const QStringList& arguments)
{
    bool workerMode = false;
    bool protocolSet = false;
    QString protocol;

    for (int index = 1; index < arguments.size(); index++) {
        const QString& argument = arguments.at(index);
        if (argument == "--argus-worker" && !workerMode) {
            workerMode = true;
        }
        else if (argument == "--protocol"
                 && !protocolSet
                 && index + 1 < arguments.size()) {
            protocolSet = true;
            protocol = arguments.at(++index);
        }
        else {
            return false;
        }
    }

    return workerMode && protocolSet && protocol == "1";
}

bool isValidPipeName(const QString& pipeName)
{
    static const QRegularExpression validName(
        QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return !pipeName.isEmpty()
        && pipeName.size() <= MaximumStartupPipeNameCharacters
        && validName.match(pipeName).hasMatch();
}

}

namespace ArgusWorker
{

bool isRequested(int argc, char* argv[])
{
    for (int index = 1; index < argc; index++) {
        if (std::strcmp(argv[index], "--argus-worker") == 0) {
            return true;
        }
    }

    return false;
}

int run(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    return runStartup(application.arguments());
}

int runStartup(const QStringList& arguments)
{
    QByteArray startupPipe =
        qgetenv(StartupPipeEnvironmentVariable);
    qunsetenv(StartupPipeEnvironmentVariable);

    if (!parseArguments(arguments)) {
        startupPipe.fill('\0');
        return ExitInvalidArguments;
    }

    if (startupPipe.isEmpty()) {
        return ExitStartupCapabilityUnavailable;
    }

    QString pipeName = QString::fromUtf8(startupPipe);
    startupPipe.fill('\0');
    if (!isValidPipeName(pipeName)) {
        pipeName.fill(QChar('\0'));
        return ExitStartupCapabilityUnavailable;
    }

    StartupChannel channel;
    StartupPayload payload;
    const StartupChannelStatus status =
        channel.receive(pipeName, payload);
    pipeName.fill(QChar('\0'));
    return status == StartupChannelStatus::Accepted
        ? ExitSuccess
        : ExitHandshakeUnavailable;
}

}
