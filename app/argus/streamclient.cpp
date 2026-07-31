#include "streamclient.h"

#include "decodedframesink.h"
#include "pairingendpoint.h"
#include "backend/nvaddress.h"
#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "settings/streamingpreferences.h"
#include "streaming/session.h"

#include <QScopeGuard>
#include <QSslCertificate>

#include <memory>
#include <algorithm>
#include <stdexcept>

namespace
{

bool sameFrameSlot(
    const ArgusWorker::StartupFrameSlotDescriptor& left,
    const ArgusWorker::StartupFrameSlotDescriptor& right)
{
    return left.mapName == right.mapName
        && left.mutexName == right.mutexName
        && left.protocolVersion == right.protocolVersion
        && left.maxWidth == right.maxWidth
        && left.maxHeight == right.maxHeight
        && left.maxPayloadBytes == right.maxPayloadBytes;
}

}

namespace ArgusWorker
{

StreamControlOutcome executeStreamControl(
    const QString& startupEndpoint,
    const StartupFrameSlotDescriptor& startupFrameSlot,
    StreamControlRequest& request,
    StreamControlResponse& response)
{
    const StartupSession session = request.session();
    const auto requestClear = qScopeGuard([&request]() {
        request.clear();
    });
    response.clear();
    if (startupEndpoint != request.endpoint()
            || !sameFrameSlot(startupFrameSlot, request.frameSlot())) {
        response.setOutcome(
            session,
            StreamControlOutcome::Rejected,
            true);
        return StreamControlOutcome::Rejected;
    }
    NvAddress address;
    const QSslCertificate serverCertificate(
        request.serverCertificate());
    if (!parsePairingEndpoint(request.endpoint(), address)
            || serverCertificate.isNull()) {
        response.setOutcome(
            session,
            StreamControlOutcome::Rejected,
            true);
        return StreamControlOutcome::Rejected;
    }

    DecodedFrameSink sink;
    if (sink.open(request.frameSlot(), session)
            != FrameSlotWriterStatus::Opened) {
        response.setOutcome(
            session,
            StreamControlOutcome::Rejected,
            true);
        return StreamControlOutcome::Rejected;
    }
    if (!installDecodedFrameSink(&sink)) {
        response.setOutcome(
            session,
            StreamControlOutcome::Rejected,
            true);
        return StreamControlOutcome::Rejected;
    }
    const auto sinkCleanup = qScopeGuard([&sink]() {
        uninstallDecodedFrameSink(&sink);
        sink.close();
    });

    StreamControlOutcome outcome = StreamControlOutcome::Unavailable;
    bool disconnectClean = true;
    try {
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
            throw std::runtime_error("Invalid Sunshine HTTPS port");
        }

        NvHTTP pinned(
            address,
            httpsPort,
            serverCertificate,
            true);
        const QString serverInfo = pinned.openConnectionToString(
            pinned.m_BaseUrlHttps,
            "serverinfo",
            nullptr,
            5000,
            NvHTTP::NVLL_NONE);
        NvHTTP::verifyResponseStatus(serverInfo);
        if (NvHTTP::getXmlString(serverInfo, "PairStatus")
                != QLatin1String("1")) {
            response.setOutcome(
                session,
                StreamControlOutcome::Rejected,
                true);
            return StreamControlOutcome::Rejected;
        }

        NvComputer computer(pinned, serverInfo);
        computer.appList = pinned.getAppList();
        auto appIterator = std::find_if(
            computer.appList.cbegin(),
            computer.appList.cend(),
            [&request](const NvApp& app) {
                return app.id == request.appId();
            });
        if (appIterator == computer.appList.cend()) {
            response.setOutcome(
                session,
                StreamControlOutcome::Rejected,
                true);
            return StreamControlOutcome::Rejected;
        }

        NvApp app = *appIterator;
        std::unique_ptr<StreamingPreferences> preferences(
            StreamingPreferences::createArgusWorker(
                request.width(),
                request.height(),
                request.framesPerSecond()));
        Session streamSession(&computer, app, preferences.get());
        if (!streamSession.initializeArgusHeadless()) {
            response.setOutcome(
                session,
                StreamControlOutcome::Rejected,
                true);
            return StreamControlOutcome::Rejected;
        }

        const Session::ArgusHeadlessOutcome sessionOutcome =
            streamSession.runArgusHeadless(
                request.firstFrameTimeoutMilliseconds());
        const DecodedFrameMetadata metadata = sink.metadata();
        switch (sessionOutcome) {
        case Session::ArgusHeadlessOutcome::FirstFrame:
            if (metadata.frameCount >= 1) {
                response.setCompleted(
                    session,
                    metadata.frameCount,
                    metadata.width,
                    metadata.height,
                    metadata.stride,
                    metadata.sequence,
                    metadata.timestampUtcTicks);
                return StreamControlOutcome::Completed;
            }
            outcome = StreamControlOutcome::Rejected;
            break;
        case Session::ArgusHeadlessOutcome::DecodeTimedOut:
            outcome = StreamControlOutcome::DecodeTimedOut;
            break;
        case Session::ArgusHeadlessOutcome::ConnectFailed:
        case Session::ArgusHeadlessOutcome::Terminated:
            outcome = StreamControlOutcome::Unavailable;
            break;
        }
    }
    catch (...) {
        outcome = StreamControlOutcome::Unavailable;
        disconnectClean = true;
    }

    response.setOutcome(session, outcome, disconnectClean);
    return outcome;
}

}
