#pragma once

#include <Arduino.h>
#include "AudioStream.h"

typedef void (*RawAudioCallback)(const int16_t *samples, size_t count);

class RawAudioSink : public MMFModule {
public:
    RawAudioSink();
    ~RawAudioSink();
    bool begin(RawAudioCallback callback);
    void end();
};
