/*
 * Copyright 2016 The Android Open Source Project
 * Copyright (C) 2015-2017 The Android Container Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CONTAINER_VIRTUAL_DISPLAY_FRAMEOUTPUT_H
#define CONTAINER_VIRTUAL_DISPLAY_FRAMEOUTPUT_H

#include <gui/BufferItem.h>
#include <gui/BufferQueue.h>
#include <gui/RingBufferConsumer.h>

namespace android {

/*
 * Support for "frames" output format.
 */
class VDFrameOutput : public RingBufferConsumer::FrameAvailableListener {
public:
    VDFrameOutput(int containerId) : mFrameAvailable(false),
        mRingBufferConsumer(NULL),
        mContainerId(containerId)
        {}

    // Create an "input surface", similar in purpose to a MediaCodec input
    // surface, that the virtual display can send buffers to.  Also configures
    // EGL with a pbuffer surface on the current thread.
    status_t createInputSurface(int width, int height,
            sp<IGraphicBufferProducer>* pBufferProducer);

    // Copy one from input to output.  If no frame is available, this will wait up to the
    // specified number of microseconds.
    //
    // Returns ETIMEDOUT if the timeout expired before we found a frame.
    status_t copyFrame(long timeoutUsec);

    void reset();

private:
    VDFrameOutput(const VDFrameOutput&);
    VDFrameOutput& operator=(const VDFrameOutput&);

    // Destruction via RefBase.
    virtual ~VDFrameOutput() {
    }

    // (overrides BufferItemConsumer::FrameAvailableListener method)
    virtual void onFrameAvailable(const BufferItem& item);

    void processBufferItem(BufferItem& item);

    // Reduces RGBA to RGB, in place.
    static void reduceRgbaToRgb(uint8_t* buf, unsigned int pixelCount);

    // Put a 32-bit value into a buffer, in little-endian byte order.
    static void setValueLE(uint8_t* buf, uint32_t value);

    // Used to wait for the FrameAvailableListener callback.
    Mutex mMutex;
    Condition mEventCond;

    // Set by the FrameAvailableListener callback.
    bool mFrameAvailable;

    // This receives frames from the virtual display and makes them available
    // as an external texture.
    sp<RingBufferConsumer> mRingBufferConsumer;

    int mContainerId;
};

}; // namespace android

#endif /* CONTAINER_VIRTUAL_DISPLAY_FRAMEOUTPUT_H */
