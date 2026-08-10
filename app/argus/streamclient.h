#pragma once

#include "startupchannel.h"

namespace ArgusWorker
{

StreamControlOutcome executeStreamControl(
    const QString& startupEndpoint,
    const QString& startupDisplayId,
    const StartupFrameSlotDescriptor& startupFrameSlot,
    StartupChannel& channel,
    StreamControlRequest& request,
    StreamControlResponse& response);

}
