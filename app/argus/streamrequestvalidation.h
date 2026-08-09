#pragma once

#include "frameslotwriter.h"
#include "startupchannel.h"

namespace ArgusWorker
{

enum class StreamRequestValidationFailure : qint32
{
    None = 0,
    InputIsolationUnavailable = 1001,
    StartupEndpointMismatch = 1002,
    StartupDisplayMissing = 1003,
    StartupDisplayMismatch = 1004,
    StartupFrameSlotMismatch = 1005,
    EndpointInvalid = 1006,
    ServerCertificateInvalid = 1007,
    FrameSlotOpenFailed = 1008,
    FrameSinkInstallFailed = 1009,
};

qint32 streamRequestValidationFailureCode(
    StreamRequestValidationFailure failure);

bool isKnownStreamRequestValidationFailureCode(qint32 code);

StreamRequestValidationFailure classifyInputIsolation(bool supported);

StreamRequestValidationFailure classifyStartupRequest(
    const QString& startupEndpoint,
    const QString& startupDisplayId,
    const StartupFrameSlotDescriptor& startupFrameSlot,
    const QString& requestEndpoint,
    const QString& requestDisplayId,
    const StartupFrameSlotDescriptor& requestFrameSlot);

StreamRequestValidationFailure classifyEndpointAndCertificate(
    bool endpointValid,
    bool certificateValid);

StreamRequestValidationFailure classifyFrameSlotOpen(
    FrameSlotWriterStatus status);

StreamRequestValidationFailure classifyFrameSinkInstall(bool installed);

}
