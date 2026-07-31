#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <array>

namespace ArgusWorker
{

inline constexpr int StartupProtocolVersion = 1;
inline constexpr int StartupNonceBytes = 32;
inline constexpr int MaximumStartupPacketBytes =
    (256 * 1024) + 8192;
inline constexpr int MaximumEndpointBytes = 2048;
inline constexpr int MaximumIdentityFormatBytes = 64;
inline constexpr int MaximumIdentityBytes = 256 * 1024;
inline constexpr int MaximumCapabilityNameBytes = 512;

using StartupGuidBytes = std::array<unsigned char, 16>;

void secureZero(void* data, qsizetype length);

struct StartupSession
{
    StartupGuidBytes machineId {};
    StartupGuidBytes attemptId {};
    StartupGuidBytes sessionId {};

    ~StartupSession();

    void clear();
    bool operator==(const StartupSession& other) const;
};

struct StartupChallenge
{
    StartupChallenge() = default;
    StartupChallenge(const StartupChallenge&) = delete;
    StartupChallenge& operator=(const StartupChallenge&) = delete;
    ~StartupChallenge();

    QByteArray nonce;
    StartupSession session;

    void clear();
};

struct StartupFrameSlotDescriptor
{
    QString mapName;
    QString mutexName;
    qint32 protocolVersion = 0;
    qint32 maxWidth = 0;
    qint32 maxHeight = 0;
    qint32 maxPayloadBytes = 0;
};

class StartupPayload
{
public:
    StartupPayload() = default;
    StartupPayload(const StartupPayload&) = delete;
    StartupPayload& operator=(const StartupPayload&) = delete;
    ~StartupPayload();

    const StartupSession& session() const;
    const QString& endpoint() const;
    const QString& identityFormat() const;
    const QByteArray& identity() const;
    QByteArray takeIdentity();
    const StartupFrameSlotDescriptor& frameSlot() const;

    void clear();

private:
    StartupSession m_session;
    QString m_endpoint;
    QString m_identityFormat;
    QByteArray m_identity;
    StartupFrameSlotDescriptor m_frameSlot;

    friend class StartupCodec;
};

enum class StartupCodecStatus
{
    Accepted,
    PacketOutOfBounds,
    InvalidChallenge,
    InvalidPayload,
    SessionMismatch,
};

class StartupCodec
{
public:
    static StartupCodecStatus decodeChallenge(
        const QByteArray& packet,
        StartupChallenge& challenge);

    static StartupCodecStatus encodeHello(
        const StartupChallenge& challenge,
        qint32 processId,
        qint64 creationTimeUtcTicks,
        QByteArray& packet);

    static StartupCodecStatus decodePayload(
        const QByteArray& packet,
        const StartupSession& expectedSession,
        StartupPayload& payload);
};

enum class StartupChannelStatus
{
    Accepted,
    AlreadyUsed,
    PipeUnavailable,
    TimedOut,
    ChannelClosed,
    PacketOutOfBounds,
    InvalidChallenge,
    InvalidPayload,
    SessionMismatch,
    ProcessMetadataUnavailable,
    IoFailure,
};

class StartupChannel
{
public:
    StartupChannel() = default;
    StartupChannel(const StartupChannel&) = delete;
    StartupChannel& operator=(const StartupChannel&) = delete;

    StartupChannelStatus receive(
        const QString& pipeName,
        StartupPayload& payload);

private:
    bool m_used = false;
};

#if defined(ARGUS_STARTUP_CHANNEL_TESTS) && defined(Q_OS_WIN)
bool exerciseExpiredDeadlineDrainForTest(void* pipeHandle);
#endif

}
