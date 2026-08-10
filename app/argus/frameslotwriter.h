#pragma once

#include "startupchannel.h"

#include <QByteArray>

namespace ArgusWorker
{

enum class FrameSlotWriterStatus
{
    Opened,
    Unsupported,
    Unavailable,
    InvalidDescriptor,
    StaleSession,
};

enum class FrameSlotPublishStatus
{
    Published,
    NotOpen,
    TimedOut,
    StaleSession,
    OutstandingFrame,
    OutOfOrder,
    ExceedsBounds,
    InvalidFrame,
    IoFailure,
};

qint32 frameSlotPublishFailureCode(FrameSlotPublishStatus status);
bool isKnownFrameSlotPublishFailureCode(qint32 code);

class FrameSlotWriter
{
public:
    FrameSlotWriter() = default;
    FrameSlotWriter(const FrameSlotWriter&) = delete;
    FrameSlotWriter& operator=(const FrameSlotWriter&) = delete;
    ~FrameSlotWriter();

    FrameSlotWriterStatus open(
        const StartupFrameSlotDescriptor& descriptor,
        const StartupSession& session);

    FrameSlotPublishStatus publish(
        qint64 sequence,
        qint64 timestampUtcTicks,
        qint32 width,
        qint32 height,
        qint32 stride,
        const QByteArray& bgraPixels);

    bool confirmConsumed(qint64 sequence);
    void discardOutstanding();

    void close();

private:
    bool headerMatches() const;
    bool lock();
    void unlock();

    StartupFrameSlotDescriptor m_descriptor;
    StartupSession m_session;
    void* m_mappingHandle = nullptr;
    void* m_mutexHandle = nullptr;
    unsigned char* m_view = nullptr;
    bool m_locked = false;
};

}
