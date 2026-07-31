#pragma once

#include "startupchannel.h"

namespace ArgusWorker
{

StreamControlOutcome executeStreamControl(
    const QString& startupEndpoint,
    const StartupFrameSlotDescriptor& startupFrameSlot,
    StreamControlRequest& request,
    StreamControlResponse& response);

}
