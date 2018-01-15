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

#ifndef VIRTUAL_DISPLAY_CONSUMER_H
#define VIRTUAL_DISPLAY_CONSUMERBASE_H

#include <gui/BufferQueue.h>

#include <ui/GraphicBuffer.h>

#include <utils/String8.h>
#include <utils/Vector.h>
#include <utils/threads.h>
#include <gui/IConsumerListener.h>

namespace android {
// ----------------------------------------------------------------------------

class String8;

/*
 * VDConsumer consumes buffers of graphics data from a BufferQueue,
 * and makes them available to ICL SVMP streaming encoder
 */
class VDConsumer : public ConsumerBase {
public:
    GLConsumer(const sp<IGraphicBufferConsumer>& bq);
    
    virtual ~VDConsumer()    {}

    // updateTexImage acquires the most recently queued buffer, and sets the
    // image contents of the target texture to it.
    //
    // This call may only be made while the OpenGL ES context to which the
    // target texture belongs is bound to the calling thread.
    //
    // This calls doGLFenceWait to ensure proper synchronization.
    status_t updateTexImage();

    // releaseTexImage releases the texture acquired in updateTexImage().
    // This is intended to be used in single buffer mode.
    //
    // This call may only be made while the OpenGL ES context to which the
    // target texture belongs is bound to the calling thread.
    status_t releaseTexImage();

    // getTimestamp retrieves the timestamp associated with the texture image
    // set by the most recent call to updateTexImage.
    //
    // The timestamp is in nanoseconds, and is monotonically increasing. Its
    // other semantics (zero point, etc) are source-dependent and should be
    // documented by the source.
    int64_t getTimestamp();

    // getFrameNumber retrieves the frame number associated with the texture
    // image set by the most recent call to updateTexImage.
    //
    // The frame number is an incrementing counter set to 0 at the creation of
    // the BufferQueue associated with this consumer.
    uint64_t getFrameNumber();

    // getCurrentBuffer returns the buffer associated with the current image.
    sp<GraphicBuffer> getCurrentBuffer() const;

    // setDefaultBufferSize is used to set the size of buffers returned by
    // requestBuffers when a with and height of zero is requested.
    // A call to setDefaultBufferSize() may trigger requestBuffers() to
    // be called from the client.
    // The width and height parameters must be no greater than the minimum of
    // GL_MAX_VIEWPORT_DIMS and GL_MAX_TEXTURE_SIZE (see: glGetIntegerv).
    // An error due to invalid dimensions might not be reported until
    // updateTexImage() is called.
    status_t setDefaultBufferSize(uint32_t width, uint32_t height);

    // set the name of the GLConsumer that will be used to identify it in log messages.
    void setName(const String8& name);

    // These functions call the corresponding BufferQueue implementation
    // so the refactoring can proceed smoothly
    status_t setDefaultBufferFormat(PixelFormat defaultFormat);
    status_t setDefaultBufferDataSpace(android_dataspace defaultDataSpace);
    status_t setConsumerUsageBits(uint32_t usage);
    status_t setTransformHint(uint32_t hint);
    status_t setMaxAcquiredBufferCount(int maxAcquiredBuffers);

protected:

private:
    // mCurrentTimestamp is the timestamp for the current texture. It
    // gets set each time updateTexImage is called.
    int64_t mCurrentTimestamp;

    // mCurrentFrameNumber is the frame counter for the current texture.
    // It gets set each time updateTexImage is called.
    uint64_t mCurrentFrameNumber;

    uint32_t mDefaultWidth, mDefaultHeight;
};

// ----------------------------------------------------------------------------
}; // namespace android

#endif // VIRTUAL_DISPLAY_CONSUMERBASE_H
