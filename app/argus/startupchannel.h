#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <array>

namespace ArgusWorker
{

inline constexpr int StartupProtocolVersion = 3;
inline constexpr int StreamProtocolVersion = 2;
inline constexpr int StartupNonceBytes = 32;
inline constexpr int MaximumStartupPacketBytes =
    (256 * 1024) + (64 * 1024) + 8192;
inline constexpr int MaximumEndpointBytes = 2048;
inline constexpr int MaximumIdentityFormatBytes = 64;
inline constexpr int MaximumIdentityBytes = 256 * 1024;
inline constexpr int MaximumServerCertificateBytes = 64 * 1024;
inline constexpr int MaximumDisplayIdBytes = 256;
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
    StartupGuidBytes frameSlotId {};
    qint32 protocolVersion = 0;
    qint32 maxWidth = 0;
    qint32 maxHeight = 0;
    qint32 maxPayloadBytes = 0;
};

enum class StreamControlTransition
{
    Accepted,
    OutstandingFrame,
    WrongSequence,
    Duplicate,
    Terminal,
};

class StreamControlSequenceFence
{
public:
    StreamControlTransition acceptFrameReady(qint64 sequence);
    StreamControlTransition acceptFrameConsumed(qint64 sequence);
    StreamControlTransition acceptTerminal();

private:
    qint64 m_lastConsumedSequence = 0;
    qint64 m_outstandingSequence = 0;
    bool m_terminal = false;
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
    const QString& displayId() const;
    const QString& identityFormat() const;
    const QByteArray& identity() const;
    QByteArray takeIdentity();
    const StartupFrameSlotDescriptor& frameSlot() const;

    void clear();

private:
    StartupSession m_session;
    QString m_endpoint;
    QString m_displayId;
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

enum class PairingControlOperation
{
    Pair = 1,
    Verify = 2,
};

enum class PairingControlOutcome
{
    Paired = 1,
    AlreadyPaired = 2,
    WrongPin = 3,
    Unavailable = 4,
    Rejected = 5,
};

class PairingControlRequest
{
public:
    PairingControlRequest() = default;
    PairingControlRequest(const PairingControlRequest&) = delete;
    PairingControlRequest& operator=(const PairingControlRequest&) = delete;
    ~PairingControlRequest();

    const StartupSession& session() const;
    PairingControlOperation operation() const;
    const QByteArray& pin() const;
    const QByteArray& serverCertificate() const;
    QByteArray takePin();
    void clear();

private:
    StartupSession m_session;
    PairingControlOperation m_operation =
        PairingControlOperation::Pair;
    QByteArray m_pin;
    QByteArray m_serverCertificate;

    friend class PairingControlCodec;
};

class PairingControlResponse
{
public:
    PairingControlResponse() = default;
    PairingControlResponse(const PairingControlResponse&) = delete;
    PairingControlResponse& operator=(const PairingControlResponse&) = delete;
    ~PairingControlResponse();

    const StartupSession& session() const;
    PairingControlOutcome outcome() const;
    const QByteArray& identityFormat() const;
    const QByteArray& identity() const;
    const QByteArray& serverCertificate() const;

    void setPaired(
        const StartupSession& session,
        const QByteArray& identityFormat,
        QByteArray identity,
        QByteArray serverCertificate);
    void setOutcome(
        const StartupSession& session,
        PairingControlOutcome outcome);
    void clear();

private:
    StartupSession m_session;
    PairingControlOutcome m_outcome =
        PairingControlOutcome::Rejected;
    QByteArray m_identityFormat;
    QByteArray m_identity;
    QByteArray m_serverCertificate;
};

enum class PairingControlCodecStatus
{
    Accepted,
    PacketOutOfBounds,
    InvalidPacket,
    SessionMismatch,
};

class PairingControlCodec
{
public:
    static PairingControlCodecStatus decodeRequest(
        const QByteArray& packet,
        const StartupSession& expectedSession,
        PairingControlRequest& request);

    static PairingControlCodecStatus encodeResponse(
        const PairingControlResponse& response,
        QByteArray& packet);
};

enum class StreamVideoCodec
{
    H264 = 1,
};

class StreamControlRequest
{
public:
    StreamControlRequest() = default;
    StreamControlRequest(const StreamControlRequest&) = delete;
    StreamControlRequest& operator=(const StreamControlRequest&) = delete;
    ~StreamControlRequest();

    const StartupSession& session() const;
    const QString& endpoint() const;
    const QByteArray& serverCertificate() const;
    const QString& displayId() const;
    qint32 appId() const;
    StreamVideoCodec codec() const;
    qint32 width() const;
    qint32 height() const;
    qint32 framesPerSecond() const;
    qint32 firstFrameTimeoutMilliseconds() const;
    const StartupFrameSlotDescriptor& frameSlot() const;
    void clear();

private:
    StartupSession m_session;
    QString m_endpoint;
    QByteArray m_serverCertificate;
    QString m_displayId;
    qint32 m_appId = 0;
    StreamVideoCodec m_codec = StreamVideoCodec::H264;
    qint32 m_width = 0;
    qint32 m_height = 0;
    qint32 m_framesPerSecond = 0;
    qint32 m_firstFrameTimeoutMilliseconds = 0;
    StartupFrameSlotDescriptor m_frameSlot;

    friend class StreamControlCodec;
};

enum class StreamControlCodecStatus
{
    Accepted,
    PacketOutOfBounds,
    InvalidPacket,
    SessionMismatch,
    NonceMismatch,
};

enum class StreamControlCommandKind
{
    FrameConsumed,
    Disconnect,
};

struct StreamControlCommand
{
    StreamControlCommandKind kind =
        StreamControlCommandKind::Disconnect;
    qint64 sequence = 0;
};

enum class StreamControlOutcome
{
    Completed = 1,
    Unavailable = 2,
    Rejected = 3,
    DecodeTimedOut = 4,
};

enum class StreamControlPhase
{
    RequestValidation = 1,
    ServerDiscovery = 2,
    PairingVerification = 3,
    ApplicationResolution = 4,
    SessionInitialization = 5,
    ConnectionStart = 6,
    FirstFrameWait = 7,
    Disconnect = 8,
    Completed = 9,
};

class StreamControlResponse
{
public:
    const StartupSession& session() const;
    StreamControlOutcome outcome() const;
    StreamControlPhase phase() const;
    qint32 failureCode() const;
    qint32 frameCount() const;
    qint32 width() const;
    qint32 height() const;
    qint32 stride() const;
    qint64 sequence() const;
    qint64 timestampUtcTicks() const;
    bool disconnectClean() const;

    void setCompleted(
        const StartupSession& session,
        qint32 frameCount,
        qint32 width,
        qint32 height,
        qint32 stride,
        qint64 sequence,
        qint64 timestampUtcTicks);
    void setOutcome(
        const StartupSession& session,
        StreamControlOutcome outcome,
        StreamControlPhase phase,
        qint32 failureCode,
        bool disconnectClean);
    void clear();

private:
    StartupSession m_session;
    StreamControlOutcome m_outcome = StreamControlOutcome::Rejected;
    StreamControlPhase m_phase = StreamControlPhase::RequestValidation;
    qint32 m_failureCode = 0;
    qint32 m_frameCount = 0;
    qint32 m_width = 0;
    qint32 m_height = 0;
    qint32 m_stride = 0;
    qint64 m_sequence = 0;
    qint64 m_timestampUtcTicks = 0;
    bool m_disconnectClean = false;
};

class StreamControlCodec
{
public:
    static StreamControlCodecStatus decodeRequest(
        const QByteArray& packet,
        const StartupSession& expectedSession,
        const QByteArray& expectedNonce,
        StreamControlRequest& request);

    static StreamControlCodecStatus encodeResponse(
        const StreamControlResponse& response,
        QByteArray& packet);

    static StreamControlCodecStatus encodeFrameReady(
        const StartupSession& session,
        const QByteArray& nonce,
        const StartupGuidBytes& frameSlotId,
        qint64 sequence,
        qint32 width,
        qint32 height,
        qint32 stride,
        qint64 timestampUtcTicks,
        QByteArray& packet);

    static StreamControlCodecStatus decodeCommand(
        const QByteArray& packet,
        const StartupSession& expectedSession,
        const QByteArray& expectedNonce,
        const StartupGuidBytes& expectedFrameSlotId,
        StreamControlCommand& command);

    static StreamControlCodecStatus encodeTerminal(
        const StreamControlResponse& response,
        const QByteArray& nonce,
        const StartupGuidBytes& frameSlotId,
        QByteArray& packet);
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
    ~StartupChannel();

    StartupChannelStatus receive(
        const QString& pipeName,
        StartupPayload& payload);

    StartupChannelStatus receivePairingRequest(
        const StartupSession& expectedSession,
        PairingControlRequest& request);

    StartupChannelStatus sendPairingResponse(
        const PairingControlResponse& response);

    StartupChannelStatus receiveStreamRequest(
        const StartupSession& expectedSession,
        StreamControlRequest& request);

    StartupChannelStatus sendStreamResponse(
        const StreamControlResponse& response);

    StartupChannelStatus sendFrameReady(
        const StartupSession& session,
        const StartupFrameSlotDescriptor& frameSlot,
        qint64 sequence,
        qint32 width,
        qint32 height,
        qint32 stride,
        qint64 timestampUtcTicks);

    StartupChannelStatus receiveStreamCommand(
        const StartupSession& expectedSession,
        const StartupFrameSlotDescriptor& frameSlot,
        StreamControlCommand& command,
        qint32 timeoutMilliseconds);

    StartupChannelStatus sendStreamTerminal(
        const StreamControlResponse& response,
        const StartupFrameSlotDescriptor& frameSlot);

private:
    bool m_used = false;
    bool m_pairingUsed = false;
    bool m_pairingResponsePending = false;
    bool m_streamUsed = false;
    bool m_streamResponsePending = false;
    bool m_streamActive = false;
    QByteArray m_channelNonce;
    void* m_pipeHandle = nullptr;
};

#if defined(ARGUS_STARTUP_CHANNEL_TESTS) && defined(Q_OS_WIN)
bool exerciseExpiredDeadlineDrainForTest(void* pipeHandle);
#endif

}
