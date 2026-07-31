#include "workerbootstrap.h"
#include "pairingclient.h"
#include "pairingidentitypackage.h"
#include "startupchannel.h"

#include <QCoreApplication>
#include <QScopeGuard>
#include <QRegularExpression>

#include <cstring>

namespace
{

constexpr int MaximumStartupPipeNameCharacters = 240;

struct ParsedArguments
{
    bool valid = false;
    bool pairingControl = false;
};

ParsedArguments parseArguments(const QStringList& arguments)
{
    bool workerMode = false;
    bool protocolSet = false;
    bool pairingControl = false;
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
        else if (argument == "--pairing-control"
                 && !pairingControl) {
            pairingControl = true;
        }
        else {
            return {};
        }
    }

    return {
        workerMode && protocolSet && protocol == "1",
        pairingControl,
    };
}

bool isValidPipeName(const QString& pipeName)
{
    static const QRegularExpression validName(
        QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return !pipeName.isEmpty()
        && pipeName.size() <= MaximumStartupPipeNameCharacters
        && validName.match(pipeName).hasMatch();
}

void discardWorkerMessage(
    QtMsgType,
    const QMessageLogContext&,
    const QString&)
{
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
    const QtMessageHandler previousMessageHandler =
        qInstallMessageHandler(discardWorkerMessage);
    const auto restoreMessageHandler = qScopeGuard(
        [previousMessageHandler]() {
            qInstallMessageHandler(previousMessageHandler);
        });
    QByteArray startupPipe =
        qgetenv(StartupPipeEnvironmentVariable);
    qunsetenv(StartupPipeEnvironmentVariable);

    const ParsedArguments parsed = parseArguments(arguments);
    if (!parsed.valid) {
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

    QString endpoint = payload.endpoint();
    const StartupSession session = payload.session();
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
    const bool identityReady = !manager->getUniqueId().isEmpty()
            && !sslConfiguration.localCertificate().isNull()
            && !sslConfiguration.privateKey().isNull();
    if (!identityReady) {
        endpoint.fill(QChar('\0'));
        endpoint.clear();
        return ExitHandshakeUnavailable;
    }
    if (!parsed.pairingControl) {
        endpoint.fill(QChar('\0'));
        endpoint.clear();
        return ExitSuccess;
    }

    PairingControlRequest request;
    if (channel.receivePairingRequest(session, request)
            != StartupChannelStatus::Accepted) {
        endpoint.fill(QChar('\0'));
        endpoint.clear();
        return ExitHandshakeUnavailable;
    }

    PairingControlResponse response;
    const PairingControlOutcome pairingOutcome =
        executePairingControl(endpoint, request, response);
    endpoint.fill(QChar('\0'));
    endpoint.clear();
    if (channel.sendPairingResponse(response)
            != StartupChannelStatus::Accepted) {
        return ExitHandshakeUnavailable;
    }
    return pairingOutcome == PairingControlOutcome::Paired
            || pairingOutcome == PairingControlOutcome::AlreadyPaired
        ? ExitSuccess
        : ExitPairingRejected;
}

}
