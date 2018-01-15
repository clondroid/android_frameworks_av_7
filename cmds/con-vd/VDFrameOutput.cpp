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

#define LOG_TAG "VDFrameOutput"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <inttypes.h>

#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>

#include "VDFrameOutput.h"

using namespace android;

struct TimestampFinder : public RingBufferConsumer::RingBufferComparator {
    typedef RingBufferConsumer::BufferInfo BufferInfo;

    enum {
        SELECT_I1 = -1,
        SELECT_I2 = 1, 
        SELECT_NEITHER = 0,
    };

    TimestampFinder(nsecs_t timestamp) : mTimestamp(timestamp) {}
    ~TimestampFinder() {}

    /**
     * Try to find the earlist frame in the ring...
     * always the first one cause screen frame comes in serial order
     */
    virtual int compare(const BufferInfo *i1,
                        const BufferInfo *i2) const {
        // Try to select non-null object first.
        if (i1 == NULL) {
            if(i2 == NULL)    return 0;

            if(i2->mTimestamp > mTimestamp)    return SELECT_I2;
            else    return 0;
        } else if (i2 == NULL) {
            if(i1 == NULL)    return 0;

            if(i1->mTimestamp > mTimestamp)    return SELECT_I1;
            else    return 0;;
        }

        // Best result: timestamp is identical
        if (i1->mTimestamp > mTimestamp)    {
            fprintf(stderr, "%s:: SELECT_I1 : %" PRIu64 "/%" PRIu64 " : %" PRIu64 "/%" PRIu64 "    %" PRIu64 "\n" ,
                    __FUNCTION__, i1->mFrameNumber, i1->mTimestamp, i2->mFrameNumber, i2->mTimestamp, (int64_t)mTimestamp);
            return SELECT_I1;
        } else    {
            if(i2->mTimestamp > mTimestamp)    {
                fprintf(stderr, "%s:: SELECT_I2 : %" PRIu64 "/%" PRIu64 " : %" PRIu64 "/%" PRIu64 "    %" PRIu64 "\n" ,
                        __FUNCTION__, i1->mFrameNumber, i1->mTimestamp, i2->mFrameNumber, i2->mTimestamp, (int64_t)mTimestamp);
            }
        }

        return 0;
    }

    nsecs_t mTimestamp;
}; // struct TimestampFinder

static TimestampFinder timestampFinder = TimestampFinder(0);

inline void VDFrameOutput::setValueLE(uint8_t* buf, uint32_t value) {
    // Since we're running on an Android device, we're (almost) guaranteed
    // to be little-endian, and (almost) guaranteed that unaligned 32-bit
    // writes will work without any performance penalty... but do it
    // byte-by-byte anyway.
    buf[0] = (uint8_t) value;
    buf[1] = (uint8_t) (value >> 8);
    buf[2] = (uint8_t) (value >> 16);
    buf[3] = (uint8_t) (value >> 24);
}

status_t VDFrameOutput::createInputSurface(int width, int height,
        sp<IGraphicBufferProducer>* pBufferProducer) {
    status_t err;
    uint32_t format = PIXEL_FORMAT_RGBA_8888;
    String8 consumerName = String8::format("VDFrameOutput-%dx%d, f%x",
                                           width, height, format);
    uint32_t consumerUsage = GRALLOC_USAGE_SW_READ_OFTEN;
    int maxAcquiredBufferCount = 8;

    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);

    consumer->setConsumerName(String8::format("Container %d VD GraphicBufferConsumer", mContainerId));
    //consumer->setMaxBufferCount(32); // BufferQueue::NUM_BUFFER_SLOTS (64)
    //producer->setMaxDequeuedBufferCount(8);
    producer->setAsyncMode(true);

    mRingBufferConsumer = new RingBufferConsumer(consumer, consumerUsage, maxAcquiredBufferCount);
    mRingBufferConsumer->setName(String8::format("Container %d Virtual Display", mContainerId));
    mRingBufferConsumer->setDefaultBufferFormat(format);
    mRingBufferConsumer->setDefaultBufferSize(width, height);
    //mBufferItemConsumer->setDefaultBufferDataSpace(dataSpace);

    mRingBufferConsumer->setFrameAvailableListener(this);

    *pBufferProducer = producer;

    return NO_ERROR;
}

status_t VDFrameOutput::copyFrame(long timeoutUsec)    {
    ALOGV("VDFrameOutput::copyFrame %ld\n", timeoutUsec);
    {
        Mutex::Autolock _l(mMutex);

        if(!mFrameAvailable) {
            nsecs_t timeoutNsec = (nsecs_t)timeoutUsec * 1000;
            int cc = mEventCond.waitRelative(mMutex, timeoutNsec);
            if (cc == -ETIMEDOUT) {
                ALOGV("    cond wait timed out....");
                return ETIMEDOUT;
            } else if (cc != 0) {
                ALOGW("    cond wait returned error.... %d", cc);
                return cc;
            }
        }

        if(!mFrameAvailable) {
            // This happens when Ctrl-C is hit.  Apparently POSIX says that the
            // pthread wait call doesn't return EINTR, treating this instead as
            // an instance of a "spurious wakeup".  We didn't get a frame, so
            // we just treat it as a timeout.
            return ETIMEDOUT;
        }

        // A frame is available.  Clear the flag for the next round.
        mFrameAvailable = false;
    }

    while(true)    {
        sp<RingBufferConsumer::PinnedBufferItem> pinnedBuffer =
                mRingBufferConsumer->pinSelectedBuffer(timestampFinder, /*waitForFence*/false);

        if(pinnedBuffer != NULL)    {
            BufferItem& item = pinnedBuffer->getBufferItem();
            timestampFinder.mTimestamp = item.mTimestamp; // very important... 
            processBufferItem(item);
        } else    {
            break;
        }
    }

    return NO_ERROR;
}

void VDFrameOutput::processBufferItem(BufferItem& item)    {
    status_t res;
    uint32_t* dataOut;
    
    if(item.mGraphicBuffer != NULL)    {
        res = item.mGraphicBuffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN,
                                        reinterpret_cast<void**>(&dataOut));
        if(res == OK)    {
            //ALOGI("%s: Successfully lock buffer: %16p on slot %04d/%" PRIu64,
            //      __FUNCTION__, dataOut, item.mSlot, item.mFrameNumber);
            fprintf(stderr, "%s: Successfully lock buffer: %16p on slot %04d/%" PRIu64 "\n",
                    __FUNCTION__, dataOut, item.mSlot, item.mFrameNumber);

            usleep(16000); // 60FPS

            item.mGraphicBuffer->unlock();
        } else    {
            ALOGE("%s: Could not lock buffer: %s (%d)",
                  __FUNCTION__, strerror(-res), res);
        }
    } else    {
        ALOGW("%s: item.mGraphicBuffer == NULL, ignore it...", __FUNCTION__);
    }
}

void VDFrameOutput::reduceRgbaToRgb(uint8_t* buf, unsigned int pixelCount) {
    // Convert RGBA to RGB.
    //
    // Unaligned 32-bit accesses are allowed on ARM, so we could do this
    // with 32-bit copies advancing at different rates (taking care at the
    // end to not go one byte over).
    const uint8_t* readPtr = buf;
    for (unsigned int i = 0; i < pixelCount; i++) {
        *buf++ = *readPtr++;
        *buf++ = *readPtr++;
        *buf++ = *readPtr++;
        readPtr++;
    }
}

// Callback; executes on arbitrary thread.
void VDFrameOutput::onFrameAvailable(const BufferItem& /* item */) {
    Mutex::Autolock _l(mMutex);

    ALOGV("VDFrameOutput::onFrameAvailable\n");

    mFrameAvailable = true;
    mEventCond.signal();
}

void VDFrameOutput::reset()    {
    if(mRingBufferConsumer != NULL)    mRingBufferConsumer.clear();
}

