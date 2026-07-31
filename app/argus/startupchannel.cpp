#include "startupchannel.h"

#include <QRegularExpression>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace
{

constexpr int StartupIoTimeoutMilliseconds = 5000;
constexpr int MaximumFrameWidth = 1920;
constexpr int MaximumFrameHeight = 1080;
constexpr int MaximumFramePayloadBytes = 8 * 1024 * 1024;
constexpr qint64 DotNetFileTimeOffsetTicks = 504911232000000000LL;

class PacketReader
{
public:
    explicit PacketReader(const QByteArray& packet)
        : m_packet(packet)
    {
    }

    bool readInt32(qint32& value)
    {
        if (!canRead(sizeof(qint32))) {
            return false;
        }

        value = qFromLittleEndian<qint32>(
            reinterpret_cast<const unsigned char*>(
                m_packet.constData() + m_offset));
        m_offset += sizeof(qint32);
        return true;
    }

    bool readInt64(qint64& value)
    {
        if (!canRead(sizeof(qint64))) {
            return false;
        }

        value = qFromLittleEndian<qint64>(
            reinterpret_cast<const unsigned char*>(
                m_packet.constData() + m_offset));
        m_offset += sizeof(qint64);
        return true;
    }

    bool readByte(unsigned char& value)
    {
        if (!canRead(1)) {
            return false;
        }

        value = static_cast<unsigned char>(
            m_packet.at(m_offset++));
        return true;
    }

    bool readBytes(qsizetype length, QByteArray& value)
    {
        if (length < 0 || !canRead(length)) {
            return false;
        }

        value = QByteArray(
            m_packet.constData() + m_offset,
            length);
        m_offset += length;
        return true;
    }

    bool readGuid(ArgusWorker::StartupGuidBytes& value)
    {
        if (!canRead(static_cast<qsizetype>(value.size()))) {
            return false;
        }

        std::memcpy(
            value.data(),
            m_packet.constData() + m_offset,
            value.size());
        m_offset += static_cast<qsizetype>(value.size());
        return true;
    }

    bool atEnd() const
    {
        return m_offset == m_packet.size();
    }

private:
    bool canRead(qsizetype length) const
    {
        return length >= 0
            && m_offset <= m_packet.size() - length;
    }

    const QByteArray& m_packet;
    qsizetype m_offset = 0;
};

void appendInt32(QByteArray& packet, qint32 value)
{
    const qint32 littleEndian = qToLittleEndian(value);
    packet.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendInt64(QByteArray& packet, qint64 value)
{
    const qint64 littleEndian = qToLittleEndian(value);
    packet.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendSession(
    QByteArray& packet,
    const ArgusWorker::StartupSession& session)
{
    packet.append(
        reinterpret_cast<const char*>(session.machineId.data()),
        static_cast<qsizetype>(session.machineId.size()));
    packet.append(
        reinterpret_cast<const char*>(session.attemptId.data()),
        static_cast<qsizetype>(session.attemptId.size()));
    packet.append(
        reinterpret_cast<const char*>(session.sessionId.data()),
        static_cast<qsizetype>(session.sessionId.size()));
}

bool readSession(
    PacketReader& reader,
    ArgusWorker::StartupSession& session)
{
    return reader.readGuid(session.machineId)
        && reader.readGuid(session.attemptId)
        && reader.readGuid(session.sessionId);
}

bool readBoundedBytes(
    PacketReader& reader,
    int maximumLength,
    QByteArray& value)
{
    qint32 length;
    return reader.readInt32(length)
        && length >= 1
        && length <= maximumLength
        && reader.readBytes(length, value);
}

bool isValidIdentityFormat(const QByteArray& format)
{
    return std::all_of(
        format.cbegin(),
        format.cend(),
        [](char value) {
            const unsigned char character =
                static_cast<unsigned char>(value);
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '.'
                || character == '_'
                || character == '-';
        });
}

bool isValidEndpoint(
    const QByteArray& endpointBytes,
    QString& endpointText)
{
    const QUrl endpoint =
        QUrl::fromEncoded(endpointBytes, QUrl::StrictMode);
    if (!endpoint.isValid()
            || endpoint.isRelative()
            || endpoint.scheme().isEmpty()
            || !endpoint.userInfo().isEmpty()
            || !endpoint.query().isEmpty()
            || !endpoint.fragment().isEmpty()) {
        return false;
    }

    endpointText = QString::fromUtf8(endpointBytes);
    return !endpointText.isEmpty();
}

bool isValidFrameSlot(
    const ArgusWorker::StartupFrameSlotDescriptor& slot)
{
    return !slot.mapName.isEmpty()
        && !slot.mutexName.isEmpty()
        && slot.protocolVersion == 1
        && slot.maxWidth >= 1
        && slot.maxWidth <= MaximumFrameWidth
        && slot.maxHeight >= 1
        && slot.maxHeight <= MaximumFrameHeight
        && slot.maxPayloadBytes >= 1
        && slot.maxPayloadBytes <= MaximumFramePayloadBytes;
}

#if defined(Q_OS_WIN)
class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : m_handle(handle)
    {
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : m_handle(std::exchange(
            other.m_handle,
            INVALID_HANDLE_VALUE))
    {
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other) {
            if (m_handle != INVALID_HANDLE_VALUE
                    && m_handle != nullptr) {
                CloseHandle(m_handle);
            }
            m_handle = std::exchange(
                other.m_handle,
                INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    ~ScopedHandle()
    {
        if (m_handle != INVALID_HANDLE_VALUE
                && m_handle != nullptr) {
            CloseHandle(m_handle);
        }
    }

    HANDLE get() const
    {
        return m_handle;
    }

    bool isValid() const
    {
        return m_handle != INVALID_HANDLE_VALUE
            && m_handle != nullptr;
    }

private:
    HANDLE m_handle;
};

DWORD remainingMilliseconds(ULONGLONG deadline)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
        return 0;
    }

    const ULONGLONG remaining = deadline - now;
    return static_cast<DWORD>(
        std::min<ULONGLONG>(
            remaining,
            std::numeric_limits<DWORD>::max()));
}

DWORD cancelAndDrainIo(
    HANDLE pipe,
    OVERLAPPED& overlapped,
    DWORD& transferred)
{
    if (!CancelIoEx(pipe, &overlapped)) {
        const DWORD cancelError = GetLastError();
        if (cancelError != ERROR_NOT_FOUND) {
            CancelIo(pipe);
        }
    }

    if (GetOverlappedResult(
            pipe,
            &overlapped,
            &transferred,
            TRUE)) {
        return ERROR_SUCCESS;
    }

    return GetLastError();
}

ArgusWorker::StartupChannelStatus waitForIo(
    HANDLE pipe,
    OVERLAPPED& overlapped,
    ULONGLONG deadline,
    DWORD& transferred,
    bool* terminalCompletionObserved = nullptr)
{
    const DWORD remaining = remainingMilliseconds(deadline);
    if (remaining == 0) {
        cancelAndDrainIo(pipe, overlapped, transferred);
        if (terminalCompletionObserved != nullptr) {
            *terminalCompletionObserved = true;
        }
        return ArgusWorker::StartupChannelStatus::TimedOut;
    }

    const DWORD wait = WaitForSingleObject(
        overlapped.hEvent,
        remaining);
    if (wait == WAIT_TIMEOUT) {
        cancelAndDrainIo(pipe, overlapped, transferred);
        if (terminalCompletionObserved != nullptr) {
            *terminalCompletionObserved = true;
        }
        return ArgusWorker::StartupChannelStatus::TimedOut;
    }

    if (wait != WAIT_OBJECT_0) {
        const DWORD error =
            cancelAndDrainIo(pipe, overlapped, transferred);
        if (terminalCompletionObserved != nullptr) {
            *terminalCompletionObserved = true;
        }
        return error == ERROR_BROKEN_PIPE
                || error == ERROR_PIPE_NOT_CONNECTED
            ? ArgusWorker::StartupChannelStatus::ChannelClosed
            : ArgusWorker::StartupChannelStatus::IoFailure;
    }

    if (!GetOverlappedResult(
            pipe,
            &overlapped,
            &transferred,
            FALSE)) {
        const DWORD error = GetLastError();
        if (terminalCompletionObserved != nullptr) {
            *terminalCompletionObserved = true;
        }
        return error == ERROR_BROKEN_PIPE
                || error == ERROR_PIPE_NOT_CONNECTED
            ? ArgusWorker::StartupChannelStatus::ChannelClosed
            : ArgusWorker::StartupChannelStatus::IoFailure;
    }

    if (terminalCompletionObserved != nullptr) {
        *terminalCompletionObserved = true;
    }
    return ArgusWorker::StartupChannelStatus::Accepted;
}

ArgusWorker::StartupChannelStatus readSome(
    HANDLE pipe,
    void* buffer,
    DWORD length,
    ULONGLONG deadline,
    DWORD& transferred)
{
    ScopedHandle event(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr));
    if (!event.isValid()) {
        return ArgusWorker::StartupChannelStatus::IoFailure;
    }

    OVERLAPPED overlapped {};
    overlapped.hEvent = event.get();
    transferred = 0;
    if (ReadFile(
            pipe,
            buffer,
            length,
            &transferred,
            &overlapped)) {
        return transferred == 0
            ? ArgusWorker::StartupChannelStatus::ChannelClosed
            : ArgusWorker::StartupChannelStatus::Accepted;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_BROKEN_PIPE
            || error == ERROR_PIPE_NOT_CONNECTED) {
        return ArgusWorker::StartupChannelStatus::ChannelClosed;
    }
    if (error != ERROR_IO_PENDING) {
        return ArgusWorker::StartupChannelStatus::IoFailure;
    }

    const ArgusWorker::StartupChannelStatus result =
        waitForIo(
            pipe,
            overlapped,
            deadline,
            transferred);
    if (result == ArgusWorker::StartupChannelStatus::Accepted
            && transferred == 0) {
        return ArgusWorker::StartupChannelStatus::ChannelClosed;
    }

    return result;
}

ArgusWorker::StartupChannelStatus readExact(
    HANDLE pipe,
    void* buffer,
    DWORD length,
    ULONGLONG deadline)
{
    auto* bytes = static_cast<unsigned char*>(buffer);
    DWORD offset = 0;
    while (offset < length) {
        DWORD transferred;
        const ArgusWorker::StartupChannelStatus result =
            readSome(
                pipe,
                bytes + offset,
                length - offset,
                deadline,
                transferred);
        if (result != ArgusWorker::StartupChannelStatus::Accepted) {
            return result;
        }

        offset += transferred;
    }

    return ArgusWorker::StartupChannelStatus::Accepted;
}

ArgusWorker::StartupChannelStatus writeSome(
    HANDLE pipe,
    const void* buffer,
    DWORD length,
    ULONGLONG deadline,
    DWORD& transferred)
{
    ScopedHandle event(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr));
    if (!event.isValid()) {
        return ArgusWorker::StartupChannelStatus::IoFailure;
    }

    OVERLAPPED overlapped {};
    overlapped.hEvent = event.get();
    transferred = 0;
    if (WriteFile(
            pipe,
            buffer,
            length,
            &transferred,
            &overlapped)) {
        return ArgusWorker::StartupChannelStatus::Accepted;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_BROKEN_PIPE
            || error == ERROR_PIPE_NOT_CONNECTED) {
        return ArgusWorker::StartupChannelStatus::ChannelClosed;
    }
    if (error != ERROR_IO_PENDING) {
        return ArgusWorker::StartupChannelStatus::IoFailure;
    }

    return waitForIo(
        pipe,
        overlapped,
        deadline,
        transferred);
}

ArgusWorker::StartupChannelStatus writeExact(
    HANDLE pipe,
    const void* buffer,
    DWORD length,
    ULONGLONG deadline)
{
    const auto* bytes =
        static_cast<const unsigned char*>(buffer);
    DWORD offset = 0;
    while (offset < length) {
        DWORD transferred;
        const ArgusWorker::StartupChannelStatus result =
            writeSome(
                pipe,
                bytes + offset,
                length - offset,
                deadline,
                transferred);
        if (result != ArgusWorker::StartupChannelStatus::Accepted) {
            return result;
        }
        if (transferred == 0) {
            return ArgusWorker::StartupChannelStatus::ChannelClosed;
        }

        offset += transferred;
    }

    return ArgusWorker::StartupChannelStatus::Accepted;
}

ArgusWorker::StartupChannelStatus readPacket(
    HANDLE pipe,
    ULONGLONG deadline,
    QByteArray& packet)
{
    qint32 lengthBytes;
    ArgusWorker::StartupChannelStatus result =
        readExact(
            pipe,
            &lengthBytes,
            sizeof(lengthBytes),
            deadline);
    if (result != ArgusWorker::StartupChannelStatus::Accepted) {
        return result;
    }

    const qint32 length = qFromLittleEndian(lengthBytes);
    if (length < 1
            || length > ArgusWorker::MaximumStartupPacketBytes) {
        return ArgusWorker::StartupChannelStatus::PacketOutOfBounds;
    }

    packet.resize(length);
    result = readExact(
        pipe,
        packet.data(),
        length,
        deadline);
    if (result != ArgusWorker::StartupChannelStatus::Accepted) {
        ArgusWorker::secureZero(packet.data(), packet.size());
        packet.clear();
    }
    return result;
}

ArgusWorker::StartupChannelStatus writePacket(
    HANDLE pipe,
    ULONGLONG deadline,
    const QByteArray& packet)
{
    if (packet.isEmpty()
            || packet.size()
                > ArgusWorker::MaximumStartupPacketBytes) {
        return ArgusWorker::StartupChannelStatus::PacketOutOfBounds;
    }

    const qint32 length =
        qToLittleEndian(static_cast<qint32>(packet.size()));
    ArgusWorker::StartupChannelStatus result =
        writeExact(
            pipe,
            &length,
            sizeof(length),
            deadline);
    if (result != ArgusWorker::StartupChannelStatus::Accepted) {
        return result;
    }

    return writeExact(
        pipe,
        packet.constData(),
        static_cast<DWORD>(packet.size()),
        deadline);
}

ArgusWorker::StartupChannelStatus connectPipe(
    const QString& pipeName,
    ULONGLONG deadline,
    ScopedHandle& pipe)
{
    const QString path =
        QStringLiteral("\\\\.\\pipe\\") + pipeName;
    const DWORD remaining = remainingMilliseconds(deadline);
    if (remaining == 0) {
        return ArgusWorker::StartupChannelStatus::TimedOut;
    }

    if (!WaitNamedPipeW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            remaining)) {
        return GetLastError() == ERROR_SEM_TIMEOUT
            ? ArgusWorker::StartupChannelStatus::TimedOut
            : ArgusWorker::StartupChannelStatus::PipeUnavailable;
    }

    ScopedHandle connected(CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL
            | FILE_FLAG_OVERLAPPED
            | SECURITY_SQOS_PRESENT
            | SECURITY_IDENTIFICATION,
        nullptr));
    if (!connected.isValid()) {
        return ArgusWorker::StartupChannelStatus::PipeUnavailable;
    }

    pipe = std::move(connected);
    return ArgusWorker::StartupChannelStatus::Accepted;
}

bool currentProcessMetadata(qint32& processId, qint64& creationTicks)
{
    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    if (!GetProcessTimes(
            GetCurrentProcess(),
            &creation,
            &exit,
            &kernel,
            &user)) {
        return false;
    }

    ULARGE_INTEGER fileTime;
    fileTime.LowPart = creation.dwLowDateTime;
    fileTime.HighPart = creation.dwHighDateTime;
    if (fileTime.QuadPart
            > static_cast<ULONGLONG>(
                std::numeric_limits<qint64>::max()
                - DotNetFileTimeOffsetTicks)) {
        return false;
    }

    const DWORD currentProcessId = GetCurrentProcessId();
    if (currentProcessId
            > static_cast<DWORD>(
                std::numeric_limits<qint32>::max())) {
        return false;
    }

    processId = static_cast<qint32>(currentProcessId);
    creationTicks =
        static_cast<qint64>(fileTime.QuadPart)
        + DotNetFileTimeOffsetTicks;
    return true;
}
#endif

ArgusWorker::StartupChannelStatus mapCodecStatus(
    ArgusWorker::StartupCodecStatus status)
{
    using ArgusWorker::StartupChannelStatus;
    using ArgusWorker::StartupCodecStatus;
    switch (status) {
    case StartupCodecStatus::Accepted:
        return StartupChannelStatus::Accepted;
    case StartupCodecStatus::PacketOutOfBounds:
        return StartupChannelStatus::PacketOutOfBounds;
    case StartupCodecStatus::InvalidChallenge:
        return StartupChannelStatus::InvalidChallenge;
    case StartupCodecStatus::InvalidPayload:
        return StartupChannelStatus::InvalidPayload;
    case StartupCodecStatus::SessionMismatch:
        return StartupChannelStatus::SessionMismatch;
    }

    return StartupChannelStatus::IoFailure;
}

}

namespace ArgusWorker
{

void secureZero(void* data, qsizetype length)
{
    volatile unsigned char* cursor =
        static_cast<volatile unsigned char*>(data);
    while (length-- > 0) {
        *cursor++ = 0;
    }
}

StartupSession::~StartupSession()
{
    clear();
}

void StartupSession::clear()
{
    secureZero(machineId.data(), machineId.size());
    secureZero(attemptId.data(), attemptId.size());
    secureZero(sessionId.data(), sessionId.size());
}

bool StartupSession::operator==(
    const StartupSession& other) const
{
    return machineId == other.machineId
        && attemptId == other.attemptId
        && sessionId == other.sessionId;
}

StartupChallenge::~StartupChallenge()
{
    clear();
}

void StartupChallenge::clear()
{
    secureZero(nonce.data(), nonce.size());
    nonce.clear();
    session.clear();
}

StartupPayload::~StartupPayload()
{
    clear();
}

const StartupSession& StartupPayload::session() const
{
    return m_session;
}

const QString& StartupPayload::endpoint() const
{
    return m_endpoint;
}

const QString& StartupPayload::identityFormat() const
{
    return m_identityFormat;
}

const QByteArray& StartupPayload::identity() const
{
    return m_identity;
}

QByteArray StartupPayload::takeIdentity()
{
    return std::move(m_identity);
}

const StartupFrameSlotDescriptor&
StartupPayload::frameSlot() const
{
    return m_frameSlot;
}

void StartupPayload::clear()
{
    m_session.clear();
    m_endpoint.fill(QChar('\0'));
    m_endpoint.clear();
    m_identityFormat.fill(QChar('\0'));
    m_identityFormat.clear();
    secureZero(m_identity.data(), m_identity.size());
    m_identity.clear();
    m_frameSlot.mapName.fill(QChar('\0'));
    m_frameSlot.mapName.clear();
    m_frameSlot.mutexName.fill(QChar('\0'));
    m_frameSlot.mutexName.clear();
    m_frameSlot.protocolVersion = 0;
    m_frameSlot.maxWidth = 0;
    m_frameSlot.maxHeight = 0;
    m_frameSlot.maxPayloadBytes = 0;
}

StartupCodecStatus StartupCodec::decodeChallenge(
    const QByteArray& packet,
    StartupChallenge& challenge)
{
    challenge.clear();
    if (packet.isEmpty()
            || packet.size() > MaximumStartupPacketBytes) {
        return StartupCodecStatus::PacketOutOfBounds;
    }

    PacketReader reader(packet);
    qint32 version;
    qint32 nonceLength;
    if (!reader.readInt32(version)
            || version != StartupProtocolVersion
            || !reader.readInt32(nonceLength)
            || nonceLength != StartupNonceBytes
            || !reader.readBytes(
                nonceLength,
                challenge.nonce)
            || !readSession(reader, challenge.session)
            || !reader.atEnd()) {
        challenge.clear();
        return StartupCodecStatus::InvalidChallenge;
    }

    return StartupCodecStatus::Accepted;
}

StartupCodecStatus StartupCodec::encodeHello(
    const StartupChallenge& challenge,
    qint32 processId,
    qint64 creationTimeUtcTicks,
    QByteArray& packet)
{
    secureZero(packet.data(), packet.size());
    packet.clear();
    if (challenge.nonce.size() != StartupNonceBytes
            || processId <= 0
            || creationTimeUtcTicks <= 0) {
        return StartupCodecStatus::InvalidChallenge;
    }

    appendInt32(packet, StartupProtocolVersion);
    appendInt32(packet, challenge.nonce.size());
    packet.append(challenge.nonce);
    appendSession(packet, challenge.session);
    appendInt32(packet, processId);
    appendInt64(packet, creationTimeUtcTicks);
    return StartupCodecStatus::Accepted;
}

StartupCodecStatus StartupCodec::decodePayload(
    const QByteArray& packet,
    const StartupSession& expectedSession,
    StartupPayload& payload)
{
    payload.clear();
    if (packet.isEmpty()
            || packet.size() > MaximumStartupPacketBytes) {
        return StartupCodecStatus::PacketOutOfBounds;
    }

    PacketReader reader(packet);
    qint32 version;
    StartupSession session;
    QByteArray endpointBytes;
    QByteArray formatBytes;
    QByteArray identity;
    unsigned char hasFrameSlot;
    QByteArray mapNameBytes;
    QByteArray mutexNameBytes;
    StartupFrameSlotDescriptor frameSlot;
    if (!reader.readInt32(version)
            || version != StartupProtocolVersion
            || !readSession(reader, session)
            || !readBoundedBytes(
                reader,
                MaximumEndpointBytes,
                endpointBytes)
            || !readBoundedBytes(
                reader,
                MaximumIdentityFormatBytes,
                formatBytes)
            || !isValidIdentityFormat(formatBytes)) {
        secureZero(endpointBytes.data(), endpointBytes.size());
        secureZero(formatBytes.data(), formatBytes.size());
        return StartupCodecStatus::InvalidPayload;
    }

    QString endpoint;
    if (!isValidEndpoint(endpointBytes, endpoint)) {
        secureZero(endpointBytes.data(), endpointBytes.size());
        secureZero(formatBytes.data(), formatBytes.size());
        return StartupCodecStatus::InvalidPayload;
    }
    secureZero(endpointBytes.data(), endpointBytes.size());

    qint32 identityLength;
    if (!reader.readInt32(identityLength)
            || identityLength < 1
            || identityLength > MaximumIdentityBytes
            || !reader.readBytes(identityLength, identity)
            || !reader.readByte(hasFrameSlot)
            || hasFrameSlot == 0
            || !readBoundedBytes(
                reader,
                MaximumCapabilityNameBytes,
                mapNameBytes)
            || !readBoundedBytes(
                reader,
                MaximumCapabilityNameBytes,
                mutexNameBytes)
            || !reader.readInt32(frameSlot.protocolVersion)
            || !reader.readInt32(frameSlot.maxWidth)
            || !reader.readInt32(frameSlot.maxHeight)
            || !reader.readInt32(frameSlot.maxPayloadBytes)
            || !reader.atEnd()) {
        secureZero(formatBytes.data(), formatBytes.size());
        secureZero(identity.data(), identity.size());
        secureZero(mapNameBytes.data(), mapNameBytes.size());
        secureZero(mutexNameBytes.data(), mutexNameBytes.size());
        endpoint.fill(QChar('\0'));
        return StartupCodecStatus::InvalidPayload;
    }

    frameSlot.mapName = QString::fromUtf8(mapNameBytes);
    frameSlot.mutexName = QString::fromUtf8(mutexNameBytes);
    secureZero(mapNameBytes.data(), mapNameBytes.size());
    secureZero(mutexNameBytes.data(), mutexNameBytes.size());
    if (!isValidFrameSlot(frameSlot)) {
        secureZero(formatBytes.data(), formatBytes.size());
        secureZero(identity.data(), identity.size());
        endpoint.fill(QChar('\0'));
        frameSlot.mapName.fill(QChar('\0'));
        frameSlot.mutexName.fill(QChar('\0'));
        return StartupCodecStatus::InvalidPayload;
    }

    if (!(session == expectedSession)) {
        secureZero(formatBytes.data(), formatBytes.size());
        secureZero(identity.data(), identity.size());
        endpoint.fill(QChar('\0'));
        frameSlot.mapName.fill(QChar('\0'));
        frameSlot.mutexName.fill(QChar('\0'));
        return StartupCodecStatus::SessionMismatch;
    }

    payload.m_session = session;
    payload.m_endpoint = endpoint;
    payload.m_identityFormat = QString::fromLatin1(formatBytes);
    payload.m_identity = identity;
    payload.m_frameSlot = frameSlot;

    secureZero(formatBytes.data(), formatBytes.size());
    secureZero(identity.data(), identity.size());
    endpoint.fill(QChar('\0'));
    frameSlot.mapName.fill(QChar('\0'));
    frameSlot.mutexName.fill(QChar('\0'));
    return StartupCodecStatus::Accepted;
}

StartupChannelStatus StartupChannel::receive(
    const QString& pipeName,
    StartupPayload& payload)
{
    if (m_used) {
        return StartupChannelStatus::AlreadyUsed;
    }
    m_used = true;
    payload.clear();

#if defined(Q_OS_WIN)
    const ULONGLONG deadline =
        GetTickCount64() + StartupIoTimeoutMilliseconds;
    ScopedHandle pipe;
    StartupChannelStatus status =
        connectPipe(pipeName, deadline, pipe);
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    QByteArray packet;
    status = readPacket(pipe.get(), deadline, packet);
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    StartupChallenge challenge;
    status = mapCodecStatus(
        StartupCodec::decodeChallenge(packet, challenge));
    secureZero(packet.data(), packet.size());
    packet.clear();
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    qint32 processId;
    qint64 creationTimeUtcTicks;
    if (!currentProcessMetadata(
            processId,
            creationTimeUtcTicks)) {
        return StartupChannelStatus::ProcessMetadataUnavailable;
    }

    status = mapCodecStatus(
        StartupCodec::encodeHello(
            challenge,
            processId,
            creationTimeUtcTicks,
            packet));
    if (status == StartupChannelStatus::Accepted) {
        status = writePacket(
            pipe.get(),
            deadline,
            packet);
    }
    secureZero(packet.data(), packet.size());
    packet.clear();
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    status = readPacket(pipe.get(), deadline, packet);
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    status = mapCodecStatus(
        StartupCodec::decodePayload(
            packet,
            challenge.session,
            payload));
    secureZero(packet.data(), packet.size());
    packet.clear();
    return status;
#else
    Q_UNUSED(pipeName);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

#if defined(ARGUS_STARTUP_CHANNEL_TESTS) && defined(Q_OS_WIN)
bool exerciseExpiredDeadlineDrainForTest(void* pipeHandle)
{
    HANDLE pipe = static_cast<HANDLE>(pipeHandle);
    ScopedHandle event(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr));
    if (!event.isValid()) {
        return false;
    }

    unsigned char byte = 0;
    DWORD transferred = 0;
    OVERLAPPED overlapped {};
    overlapped.hEvent = event.get();
    if (ReadFile(
            pipe,
            &byte,
            sizeof(byte),
            &transferred,
            &overlapped)
            || GetLastError() != ERROR_IO_PENDING) {
        return false;
    }

    bool terminalCompletionObserved = false;
    const StartupChannelStatus status = waitForIo(
        pipe,
        overlapped,
        GetTickCount64(),
        transferred,
        &terminalCompletionObserved);
    secureZero(&byte, sizeof(byte));
    return status == StartupChannelStatus::TimedOut
        && terminalCompletionObserved;
}
#endif

}
