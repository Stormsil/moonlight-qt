#include "workerbootstrap.h"
#include "pairingidentitypackage.h"
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
    if (status != StartupChannelStatus::Accepted) {
        return ExitHandshakeUnavailable;
    }

    QString identityFormat = payload.identityFormat();
    IdentityManager::ProcessIdentity identity;
    const PairingIdentityPackageStatus packageStatus =
        PairingIdentityPackage::decode(
            identityFormat,
            payload.takeIdentity(),
            identity);
    identityFormat.fill(QChar('\0'));
    identityFormat.clear();
    payload.clear();
    if (packageStatus != PairingIdentityPackageStatus::Accepted
            || IdentityManager::installProcessIdentity(
                std::move(identity))
                != IdentityManager::ProcessIdentityInstallResult::Installed) {
        return ExitHandshakeUnavailable;
    }

    IdentityManager* manager = IdentityManager::get();
    const QSslConfiguration sslConfiguration =
        manager->getSslConfig();
    return !manager->getUniqueId().isEmpty()
            && !sslConfiguration.localCertificate().isNull()
            && !sslConfiguration.privateKey().isNull()
        ? ExitSuccess
        : ExitHandshakeUnavailable;
}

}
