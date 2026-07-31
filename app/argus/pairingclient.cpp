#include "pairingclient.h"

#include "pairingendpoint.h"
#include "pairingidentitypackage.h"
#include "backend/nvaddress.h"
#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "backend/nvpairingmanager.h"

#include <QScopeGuard>
#include <QSslCertificate>
#include <utility>

namespace
{

ArgusWorker::PairingControlOutcome executePair(
    NvAddress address,
    ArgusWorker::PairingControlRequest& request,
    ArgusWorker::PairingControlResponse& response)
{
    NvHTTP discovery(
        address,
        0,
        QSslCertificate(),
        true);
    const QString serverInfo = discovery.getServerInfo(
        NvHTTP::NVLL_NONE);
    const QString appVersion = NvHTTP::getXmlString(
        serverInfo,
        "appversion");
    if (appVersion.isEmpty()
            || NvHTTP::getXmlString(serverInfo, "PairStatus")
                == QLatin1String("1")) {
        return ArgusWorker::PairingControlOutcome::Rejected;
    }

    NvComputer computer(discovery, serverInfo);
    NvPairingManager pairing(&computer);
    QByteArray pin = request.takePin();
    const auto pinZero = qScopeGuard([&pin]() {
        ArgusWorker::secureZero(pin.data(), pin.size());
        pin.clear();
    });
    QSslCertificate serverCertificate;
    const NvPairingManager::PairState pairState = pairing.pair(
        appVersion,
        pin,
        serverCertificate);
    if (pairState == NvPairingManager::PIN_WRONG) {
        return ArgusWorker::PairingControlOutcome::WrongPin;
    }
    if (pairState != NvPairingManager::PAIRED
            || serverCertificate.isNull()) {
        return pairState == NvPairingManager::ALREADY_IN_PROGRESS
            ? ArgusWorker::PairingControlOutcome::Rejected
            : ArgusWorker::PairingControlOutcome::Unavailable;
    }

    QByteArray identityPackage;
    QByteArray serverCertificatePem = serverCertificate.toPem();
    if (ArgusWorker::PairingIdentityPackage::encode(
            *IdentityManager::get(),
            identityPackage)
            != ArgusWorker::PairingIdentityPackageStatus::Accepted
            || serverCertificatePem.isEmpty()
            || serverCertificatePem.size()
                > ArgusWorker::MaximumServerCertificateBytes) {
        ArgusWorker::secureZero(
            identityPackage.data(),
            identityPackage.size());
        ArgusWorker::secureZero(
            serverCertificatePem.data(),
            serverCertificatePem.size());
        return ArgusWorker::PairingControlOutcome::Rejected;
    }

    response.setPaired(
        request.session(),
        ArgusWorker::PairingIdentityFormat,
        std::move(identityPackage),
        std::move(serverCertificatePem));
    return ArgusWorker::PairingControlOutcome::Paired;
}

ArgusWorker::PairingControlOutcome executeVerify(
    NvAddress address,
    const ArgusWorker::PairingControlRequest& request)
{
    const QSslCertificate serverCertificate(
        request.serverCertificate());
    if (serverCertificate.isNull()) {
        return ArgusWorker::PairingControlOutcome::Rejected;
    }

    NvHTTP discovery(
        address,
        0,
        QSslCertificate(),
        true);
    const QString discoveryInfo = discovery.getServerInfo(
        NvHTTP::NVLL_NONE);
    bool portValid = false;
    const uint16_t httpsPort = NvHTTP::getXmlString(
        discoveryInfo,
        "HttpsPort").toUShort(&portValid);
    if (!portValid || httpsPort == 0) {
        return ArgusWorker::PairingControlOutcome::Rejected;
    }

    NvHTTP pinned(
        address,
        httpsPort,
        serverCertificate,
        true);
    // Do not call getServerInfo() here: the ordinary interactive helper may
    // fall back to plaintext HTTP after a certificate error. Verification of
    // a persisted Argus binding is deliberately HTTPS-only.
    const QString serverInfo = pinned.openConnectionToString(
        pinned.m_BaseUrlHttps,
        "serverinfo",
        nullptr,
        5000,
        NvHTTP::NVLL_NONE);
    NvHTTP::verifyResponseStatus(serverInfo);
    return NvHTTP::getXmlString(serverInfo, "PairStatus")
            == QLatin1String("1")
        ? ArgusWorker::PairingControlOutcome::AlreadyPaired
        : ArgusWorker::PairingControlOutcome::Rejected;
}

}

namespace ArgusWorker
{

PairingControlOutcome executePairingControl(
    const QString& endpoint,
    PairingControlRequest& request,
    PairingControlResponse& response)
{
    const auto requestClear = qScopeGuard([&request]() {
        request.clear();
    });
    response.clear();
    const StartupSession session = request.session();
    NvAddress address;
    if (!parsePairingEndpoint(endpoint, address)) {
        response.setOutcome(session, PairingControlOutcome::Rejected);
        return PairingControlOutcome::Rejected;
    }

    PairingControlOutcome outcome;
    try {
        outcome = request.operation() == PairingControlOperation::Pair
            ? executePair(address, request, response)
            : executeVerify(address, request);
    }
    catch (...) {
        outcome = PairingControlOutcome::Unavailable;
    }

    if (outcome != PairingControlOutcome::Paired) {
        response.setOutcome(session, outcome);
    }
    return outcome;
}

}
