#include "streamrequestvalidation.h"

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

qint32 streamRequestValidationFailureCode(
    StreamRequestValidationFailure failure)
{
    return static_cast<qint32>(failure);
}

bool isKnownStreamRequestValidationFailureCode(qint32 code)
{
    switch (static_cast<StreamRequestValidationFailure>(code)) {
    case StreamRequestValidationFailure::InputIsolationUnavailable:
    case StreamRequestValidationFailure::StartupEndpointMismatch:
    case StreamRequestValidationFailure::StartupDisplayMissing:
    case StreamRequestValidationFailure::StartupDisplayMismatch:
    case StreamRequestValidationFailure::StartupFrameSlotMismatch:
    case StreamRequestValidationFailure::EndpointInvalid:
    case StreamRequestValidationFailure::ServerCertificateInvalid:
    case StreamRequestValidationFailure::FrameSlotOpenFailed:
    case StreamRequestValidationFailure::FrameSinkInstallFailed:
        return true;
    case StreamRequestValidationFailure::None:
        return false;
    }

    return false;
}

StreamRequestValidationFailure classifyInputIsolation(bool supported)
{
    return supported
        ? StreamRequestValidationFailure::None
        : StreamRequestValidationFailure::InputIsolationUnavailable;
}

StreamRequestValidationFailure classifyStartupRequest(
    const QString& startupEndpoint,
    const QString& startupDisplayId,
    const StartupFrameSlotDescriptor& startupFrameSlot,
    const QString& requestEndpoint,
    const QString& requestDisplayId,
    const StartupFrameSlotDescriptor& requestFrameSlot)
{
    if (startupEndpoint != requestEndpoint) {
        return StreamRequestValidationFailure::StartupEndpointMismatch;
    }
    if (startupDisplayId.isEmpty()) {
        return StreamRequestValidationFailure::StartupDisplayMissing;
    }
    if (startupDisplayId != requestDisplayId) {
        return StreamRequestValidationFailure::StartupDisplayMismatch;
    }
    if (!sameFrameSlot(startupFrameSlot, requestFrameSlot)) {
        return StreamRequestValidationFailure::StartupFrameSlotMismatch;
    }

    return StreamRequestValidationFailure::None;
}

StreamRequestValidationFailure classifyEndpointAndCertificate(
    bool endpointValid,
    bool certificateValid)
{
    if (!endpointValid) {
        return StreamRequestValidationFailure::EndpointInvalid;
    }
    if (!certificateValid) {
        return StreamRequestValidationFailure::ServerCertificateInvalid;
    }

    return StreamRequestValidationFailure::None;
}

StreamRequestValidationFailure classifyFrameSlotOpen(
    FrameSlotWriterStatus status)
{
    return status == FrameSlotWriterStatus::Opened
        ? StreamRequestValidationFailure::None
        : StreamRequestValidationFailure::FrameSlotOpenFailed;
}

StreamRequestValidationFailure classifyFrameSinkInstall(bool installed)
{
    return installed
        ? StreamRequestValidationFailure::None
        : StreamRequestValidationFailure::FrameSinkInstallFailed;
}

}
