#include "pairingendpoint.h"

#include "backend/nvaddress.h"

#include <QUrl>

namespace ArgusWorker
{

bool parsePairingEndpoint(
    const QString& endpointText,
    NvAddress& address)
{
    const QUrl endpoint(endpointText, QUrl::StrictMode);
    const int port = endpoint.port(80);
    if (!endpoint.isValid()
            || endpoint.scheme() != QLatin1String("http")
            || endpoint.host().isEmpty()
            || port < 1
            || port > 65535
            || (!endpoint.path().isEmpty()
                && endpoint.path() != QLatin1String("/"))
            || !endpoint.userInfo().isEmpty()
            || !endpoint.query().isEmpty()
            || !endpoint.fragment().isEmpty()) {
        return false;
    }

    address = NvAddress(
        endpoint.host(),
        static_cast<uint16_t>(port));
    return true;
}

}
