#pragma once

#include "backend/identitymanager.h"

#include <QByteArray>
#include <QString>

namespace ArgusWorker
{

inline constexpr char PairingIdentityFormat[] =
    "moonlight-qt.identity-v1";

enum class PairingIdentityPackageStatus
{
    Accepted,
    UnsupportedFormat,
    InvalidPackage,
    InvalidIdentity,
};

class PairingIdentityPackage
{
public:
    static PairingIdentityPackageStatus decode(
        const QString& format,
        QByteArray package,
        IdentityManager::ProcessIdentity& identity);
};

}
