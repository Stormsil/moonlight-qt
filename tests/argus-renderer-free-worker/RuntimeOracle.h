#pragma once

struct AVFrame;

namespace ArgusRendererFreeOracle
{

bool isRequested(int argc, char* argv[]);
int run(int argc, char* argv[]);
void publishDecodedProbeFrame(AVFrame* frame);

}
