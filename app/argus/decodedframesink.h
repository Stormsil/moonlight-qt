#pragma once

#include "frameslotwriter.h"

#include <QMutex>

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
    bool publish(AVFrame* frame);
    DecodedFrameMetadata metadata() const;

private:
    mutable QMutex m_mutex;
    FrameSlotWriter m_writer;
    SwsContext* m_swsContext = nullptr;
    DecodedFrameMetadata m_metadata;
};

bool installDecodedFrameSink(DecodedFrameSink* sink);
void uninstallDecodedFrameSink(DecodedFrameSink* sink);
void publishDecodedFrame(AVFrame* frame);
DecodedFrameMetadata activeDecodedFrameMetadata();

}
