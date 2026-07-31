#pragma once

#include <QString>

class NvAddress;

namespace ArgusWorker
{

bool parsePairingEndpoint(
    const QString& endpointText,
    NvAddress& address);

}
