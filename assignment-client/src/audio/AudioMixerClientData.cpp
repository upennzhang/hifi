//
//  AudioMixerClientData.cpp
//  hifi
//
//  Created by Stephen Birarda on 10/18/13.
//  Copyright (c) 2013 HighFidelity, Inc. All rights reserved.
//

#include <QDebug>

#include <PacketHeaders.h>
#include <UUID.h>

#include "InjectedAudioRingBuffer.h"

#include "AudioMixerClientData.h"

AudioMixerClientData::AudioMixerClientData() :
    _ringBuffers(),
    _nextOutputLoudness(0)
{
    
}

AudioMixerClientData::~AudioMixerClientData() {
    for (unsigned int i = 0; i < _ringBuffers.size(); i++) {
        // delete this attached PositionalAudioRingBuffer
        delete _ringBuffers[i];
    }
}

AvatarAudioRingBuffer* AudioMixerClientData::getAvatarAudioRingBuffer() const {
    for (unsigned int i = 0; i < _ringBuffers.size(); i++) {
        if (_ringBuffers[i]->getType() == PositionalAudioRingBuffer::Microphone) {
            return (AvatarAudioRingBuffer*) _ringBuffers[i];
        }
    }

    // no AvatarAudioRingBuffer found - return NULL
    return NULL;
}

int AudioMixerClientData::parseData(const QByteArray& packet) {
    PacketType packetType = packetTypeForPacket(packet);
    if (packetType == PacketTypeMicrophoneAudioWithEcho
        || packetType == PacketTypeMicrophoneAudioNoEcho
        || packetType == PacketTypeSilentAudioFrame) {

        // grab the AvatarAudioRingBuffer from the vector (or create it if it doesn't exist)
        AvatarAudioRingBuffer* avatarRingBuffer = getAvatarAudioRingBuffer();

        if (!avatarRingBuffer) {
            // we don't have an AvatarAudioRingBuffer yet, so add it
            avatarRingBuffer = new AvatarAudioRingBuffer();
            _ringBuffers.push_back(avatarRingBuffer);
        }

        // ask the AvatarAudioRingBuffer instance to parse the data
        avatarRingBuffer->parseData(packet);
    } else {
        // this is injected audio

        // grab the stream identifier for this injected audio
        QUuid streamIdentifier = QUuid::fromRfc4122(packet.mid(numBytesForPacketHeader(packet), NUM_BYTES_RFC4122_UUID));

        InjectedAudioRingBuffer* matchingInjectedRingBuffer = NULL;

        for (unsigned int i = 0; i < _ringBuffers.size(); i++) {
            if (_ringBuffers[i]->getType() == PositionalAudioRingBuffer::Injector
                && ((InjectedAudioRingBuffer*) _ringBuffers[i])->getStreamIdentifier() == streamIdentifier) {
                matchingInjectedRingBuffer = (InjectedAudioRingBuffer*) _ringBuffers[i];
            }
        }

        if (!matchingInjectedRingBuffer) {
            // we don't have a matching injected audio ring buffer, so add it
            matchingInjectedRingBuffer = new InjectedAudioRingBuffer(streamIdentifier);
            _ringBuffers.push_back(matchingInjectedRingBuffer);
        }

        matchingInjectedRingBuffer->parseData(packet);
    }

    return 0;
}

void AudioMixerClientData::checkBuffersBeforeFrameSend(int jitterBufferLengthSamples,
                                                       float& currentMinLoudness,
                                                       float& currentMaxLoudness) {
    for (unsigned int i = 0; i < _ringBuffers.size(); i++) {
        if (_ringBuffers[i]->shouldBeAddedToMix(jitterBufferLengthSamples)) {
            // this is a ring buffer that is ready to go
            // set its flag so we know to push its buffer when all is said and done
            _ringBuffers[i]->setWillBeAddedToMix(true);
            
            const int TRAILING_AVERAGE_FRAMES = 100;
            const float CURRENT_FRAME_RATIO = 1 / TRAILING_AVERAGE_FRAMES;
            const float PREVIOUS_FRAMES_RATIO = 1 - CURRENT_FRAME_RATIO;
            
            // calculate the average loudness for the next NETWORK_BUFFER_LENGTH_SAMPLES_PER_CHANNEL
            // that would be mixed in
            _nextOutputLoudness = (_nextOutputLoudness * PREVIOUS_FRAMES_RATIO) + (CURRENT_FRAME_RATIO * _ringBuffers[i]->averageLoudnessForBoundarySamples(NETWORK_BUFFER_LENGTH_SAMPLES_PER_CHANNEL));
            
            if (_nextOutputLoudness != 0 && _nextOutputLoudness < currentMinLoudness) {
                currentMinLoudness = _nextOutputLoudness;
            }
            
            if (_nextOutputLoudness > currentMaxLoudness) {
                currentMaxLoudness = _nextOutputLoudness;
            }
        }
    }
}

void AudioMixerClientData::pushBuffersAfterFrameSend() {
    for (unsigned int i = 0; i < _ringBuffers.size(); i++) {
        // this was a used buffer, push the output pointer forwards
        PositionalAudioRingBuffer* audioBuffer = _ringBuffers[i];

        if (audioBuffer->willBeAddedToMix()) {
            audioBuffer->shiftReadPosition(NETWORK_BUFFER_LENGTH_SAMPLES_PER_CHANNEL);

            audioBuffer->setWillBeAddedToMix(false);
        } else if (audioBuffer->getType() == PositionalAudioRingBuffer::Injector
                   && audioBuffer->hasStarted() && audioBuffer->isStarved()) {
            // this is an empty audio buffer that has starved, safe to delete
            delete audioBuffer;
            _ringBuffers.erase(_ringBuffers.begin() + i);
        }
    }
}
