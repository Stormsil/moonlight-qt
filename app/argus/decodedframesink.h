#pragma once

#include "frameslotwriter.h"

#include <QMutex>
#include <QWaitCondition>

struct AVFrame;
struct SwsContext;

namespace ArgusWorker
{

struct DecodedFrameMetadata
{
    qint64 sequence = 0;
    qint64 timestampUtcTicks = 0;
    qint32 width = 0;
    qint32 height = 0;
    qint32 stride = 0;
    qint32 frameCount = 0;
};

enum class DecodedFrameWaitOutcome
{
    FrameReady,
    Control,
    TimedOut,
    Failed,
    Closed,
};

class DecodedFrameSink
{
public:
    DecodedFrameSink() = default;
    DecodedFrameSink(const DecodedFrameSink&) = delete;
    DecodedFrameSink& operator=(const DecodedFrameSink&) = delete;
    ~DecodedFrameSink();

    FrameSlotWriterStatus open(
        const StartupFrameSlotDescriptor& descriptor,
        const StartupSession& session);
    void close();
    FrameSlotPublishStatus publish(AVFrame* frame);
    DecodedFrameMetadata metadata() const;
    DecodedFrameWaitOutcome waitForFrameOrControl(
        qint64 afterSequence,
        qint32 timeoutMilliseconds,
        DecodedFrameMetadata& metadata);
    void notifyControl();
    void clearControlNotification();
    bool acknowledgeConsumed(qint64 sequence);

private:
    mutable QMutex m_mutex;
    QWaitCondition m_changed;
    FrameSlotWriter m_writer;
    SwsContext* m_swsContext = nullptr;
    DecodedFrameMetadata m_metadata;
    qint64 m_outstandingSequence = 0;
    qint32 m_failureCode = 0;
    bool m_controlNotified = false;
    bool m_closed = true;
};

bool installDecodedFrameSink(DecodedFrameSink* sink);
void uninstallDecodedFrameSink(DecodedFrameSink* sink);
FrameSlotPublishStatus publishDecodedFrame(AVFrame* frame);
DecodedFrameMetadata activeDecodedFrameMetadata();
qint32 activeDecodedFrameFailureCode();
DecodedFrameWaitOutcome waitForActiveDecodedFrameAfter(
    qint64 afterSequence,
    qint32 timeoutMilliseconds,
    DecodedFrameMetadata& metadata);

}
