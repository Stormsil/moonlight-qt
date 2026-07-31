#pragma once

#include "startupchannel.h"

#include <QString>

namespace ArgusWorker
{

PairingControlOutcome executePairingControl(
    const QString& endpoint,
    PairingControlRequest& request,
    PairingControlResponse& response);

}
