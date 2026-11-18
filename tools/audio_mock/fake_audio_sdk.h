#pragma once
#include <cstdint>
struct AXInitParams { unsigned renderer{}, pipeline{}; };
struct AXVoiceOffsets {
    unsigned dataType{}, loopingEnabled{}, loopOffset{}, endOffset{}, currentOffset{};
    void *data{};
};
struct AXVoice { AXVoiceOffsets offsets; bool playing{}; };
struct AXVoiceVeData { unsigned volume{}; };
struct AXVoiceDeviceMixData { struct Bus { unsigned volume{}; } bus[4]; };
enum { AX_INIT_RENDERER_48KHZ, AX_INIT_PIPELINE_SINGLE, AX_DEVICE_TYPE_TV, AX_DEVICE_TYPE_DRC,
       AX_VOICE_STATE_PLAYING, AX_VOICE_STATE_STOPPED, AX_VOICE_FORMAT_LPCM16,
       AX_VOICE_LOOP_ENABLED, AX_VOICE_SRC_TYPE_LINEAR };
void AXInitWithParams(const AXInitParams *);
void AXQuit();
AXVoice *AXAcquireVoice(unsigned, void *, void *);
void AXFreeVoice(AXVoice *);
void AXVoiceBegin(AXVoice *);
void AXVoiceEnd(AXVoice *);
void AXSetVoiceType(AXVoice *, unsigned);
void AXSetVoiceState(AXVoice *, unsigned);
void AXSetVoiceVe(AXVoice *, const AXVoiceVeData *);
void AXSetVoiceDeviceMix(AXVoice *, unsigned, unsigned, const AXVoiceDeviceMixData *);
void AXSetVoiceOffsets(AXVoice *, const AXVoiceOffsets *);
void AXGetVoiceOffsets(AXVoice *, AXVoiceOffsets *);
void AXSetVoiceSrcType(AXVoice *, unsigned);
void AXSetVoiceSrcRatio(AXVoice *, float);
unsigned AXGetInputSamplesPerSec();
void DCFlushRange(void *, unsigned);
