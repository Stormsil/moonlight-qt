#include "streamclient.h"

#include "decodedframesink.h"
#include "pairingendpoint.h"
#include "streamrequestvalidation.h"
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

namespace ArgusWorker
{

namespace
{

StreamControlOutcome rejectRequest(
    const StartupSession& session,
    StreamControlResponse& response,
    StreamRequestValidationFailure failure)
{
    response.setOutcome(
        session,
        StreamControlOutcome::Rejected,
        StreamControlPhase::RequestValidation,
        streamRequestValidationFailureCode(failure),
        true);
    return StreamControlOutcome::Rejected;
}

}

StreamControlOutcome executeStreamControl(
    const QString& startupEndpoint,
    const QString& startupDisplayId,
    const StartupFrameSlotDescriptor& startupFrameSlot,
    StreamControlRequest& request,
    StreamControlResponse& response)
{
    const StartupSession session = request.session();
    const auto requestClear = qScopeGuard([&request]() {
        request.clear();
    });
    response.clear();
    StreamRequestValidationFailure validationFailure =
        classifyStartupRequest(
            startupEndpoint,
            startupDisplayId,
            startupFrameSlot,
            request.endpoint(),
            request.displayId(),
            request.frameSlot());
    if (validationFailure != StreamRequestValidationFailure::None) {
        return rejectRequest(session, response, validationFailure);
    }
    NvAddress address;
    const QSslCertificate serverCertificate(
        request.serverCertificate());
    validationFailure = classifyEndpointAndCertificate(
        parsePairingEndpoint(request.endpoint(), address),
        !serverCertificate.isNull());
    if (validationFailure != StreamRequestValidationFailure::None) {
        return rejectRequest(session, response, validationFailure);
    }

    DecodedFrameSink sink;
    validationFailure = classifyFrameSlotOpen(
        sink.open(request.frameSlot(), session));
    if (validationFailure != StreamRequestValidationFailure::None) {
        return rejectRequest(session, response, validationFailure);
    }
    validationFailure = classifyFrameSinkInstall(
        installDecodedFrameSink(&sink));
    if (validationFailure != StreamRequestValidationFailure::None) {
        return rejectRequest(session, response, validationFailure);
    }
    const auto sinkCleanup = qScopeGuard([&sink]() {
        uninstallDecodedFrameSink(&sink);
        sink.close();
    });

    StreamControlOutcome outcome = StreamControlOutcome::Unavailable;
    StreamControlPhase phase = StreamControlPhase::ServerDiscovery;
    qint32 failureCode = 0;
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
        phase = StreamControlPhase::PairingVerification;
        if (NvHTTP::getXmlString(serverInfo, "PairStatus")
                != QLatin1String("1")) {
            response.setOutcome(
                session,
                StreamControlOutcome::Rejected,
                phase,
                0,
                true);
            return StreamControlOutcome::Rejected;
        }

        NvComputer computer(pinned, serverInfo);
        phase = StreamControlPhase::ApplicationResolution;
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
                phase,
                0,
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
        phase = StreamControlPhase::SessionInitialization;
        if (!streamSession.initializeArgusHeadless()) {
            response.setOutcome(
                session,
                StreamControlOutcome::Rejected,
                phase,
                0,
                true);
            return StreamControlOutcome::Rejected;
        }

        phase = StreamControlPhase::ConnectionStart;
        const Session::ArgusHeadlessOutcome sessionOutcome =
            streamSession.runArgusHeadless(
                request.firstFrameTimeoutMilliseconds());
        failureCode = streamSession.argusFailureCode();
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
            phase = StreamControlPhase::FirstFrameWait;
            break;
        case Session::ArgusHeadlessOutcome::DecodeTimedOut:
            outcome = StreamControlOutcome::DecodeTimedOut;
            phase = StreamControlPhase::FirstFrameWait;
            break;
        case Session::ArgusHeadlessOutcome::SinkFailed:
            outcome = StreamControlOutcome::Unavailable;
            phase = StreamControlPhase::FirstFrameWait;
            break;
        case Session::ArgusHeadlessOutcome::ConnectFailed:
            phase = StreamControlPhase::ConnectionStart;
            outcome = StreamControlOutcome::Unavailable;
            break;
        case Session::ArgusHeadlessOutcome::Terminated:
            phase = StreamControlPhase::FirstFrameWait;
            outcome = StreamControlOutcome::Unavailable;
            break;
        }
    }
    catch (...) {
        outcome = StreamControlOutcome::Unavailable;
        disconnectClean = true;
    }

    response.setOutcome(
        session,
        outcome,
        phase,
        failureCode,
        disconnectClean);
    return outcome;
}

}
