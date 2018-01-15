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

#define LOG_TAG "VDConsumer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include <gui/BufferItem.h>
#include <gui/GLConsumer.h>
#include <gui/IGraphicBufferAlloc.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>

#include <private/gui/ComposerService.h>
#include <private/gui/SyncFeatures.h>

#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/Trace.h>

namespace android {

// Macros for including the VDConsumer name in log messages
#define VDC_LOGV(x, ...) ALOGV("[%s] " x, mName.string(), ##__VA_ARGS__)
#define VDC_LOGD(x, ...) ALOGD("[%s] " x, mName.string(), ##__VA_ARGS__)
#define VDC_LOGI(x, ...) ALOGI("[%s] " x, mName.string(), ##__VA_ARGS__)
#define VDC_LOGW(x, ...) ALOGW("[%s] " x, mName.string(), ##__VA_ARGS__)
#define VDC_LOGE(x, ...) ALOGE("[%s] " x, mName.string(), ##__VA_ARGS__)

VDConsumer::VDConsumer(const sp<IGraphicBufferConsumer>& bq) :
    ConsumerBase(bq, false),
    mCurrentTimestamp(0),
    mCurrentFrameNumber(0),
    mDefaultWidth(1),
    mDefaultHeight(1)
{
    // the buffer will be accessed from software
    // meaning application will touch pixels directly
    mConsumer->setConsumerUsageBits(DEFAULT_USAGE_FLAGS |
                                    GRALLOC_USAGE_SW_READ_OFTEN);
}

status_t VDConsumer::updateTexImage() {
    ATRACE_CALL();
    VDC_LOGV("updateTexImage");
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        VDC_LOGE("updateTexImage: VDConsumer is abandoned!");
        return NO_INIT;
    }

    // Make sure the EGL state is the same as in previous calls.
    status_t err = checkAndUpdateEglStateLocked();
    if (err != NO_ERROR) {
        return err;
    }

    BufferItem item;

    // Acquire the next buffer.
    // In asynchronous mode the list is guaranteed to be one buffer
    // deep, while in synchronous mode we use the oldest buffer.
    err = acquireBufferLocked(&item, 0);
    if (err != NO_ERROR) {
        if (err == BufferQueue::NO_BUFFER_AVAILABLE) {
            // We always bind the texture even if we don't update its contents.
            GLC_LOGV("updateTexImage: no buffers were available");
            glBindTexture(mTexTarget, mTexName);
            err = NO_ERROR;
        } else {
            GLC_LOGE("updateTexImage: acquire failed: %s (%d)",
                strerror(-err), err);
        }
        return err;
    }

    // Release the previous buffer.
    err = updateAndReleaseLocked(item);
    if (err != NO_ERROR) {
        // We always bind the texture.
        glBindTexture(mTexTarget, mTexName);
        return err;
    }

    // Bind the new buffer to the GL texture, and wait until it's ready.
    return bindTextureImageLocked();
}


status_t GLConsumer::releaseTexImage() {
    ATRACE_CALL();
    GLC_LOGV("releaseTexImage");
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        GLC_LOGE("releaseTexImage: GLConsumer is abandoned!");
        return NO_INIT;
    }

    // Make sure the EGL state is the same as in previous calls.
    status_t err = NO_ERROR;

    if (mAttached) {
        err = checkAndUpdateEglStateLocked(true);
        if (err != NO_ERROR) {
            return err;
        }
    } else {
        // if we're detached, no need to validate EGL's state -- we won't use it.
    }

    // Update the GLConsumer state.
    int buf = mCurrentTexture;
    if (buf != BufferQueue::INVALID_BUFFER_SLOT) {

        GLC_LOGV("releaseTexImage: (slot=%d, mAttached=%d)", buf, mAttached);

        if (mAttached) {
            // Do whatever sync ops we need to do before releasing the slot.
            err = syncForReleaseLocked(mEglDisplay);
            if (err != NO_ERROR) {
                GLC_LOGE("syncForReleaseLocked failed (slot=%d), err=%d", buf, err);
                return err;
            }
        } else {
            // if we're detached, we just use the fence that was created in detachFromContext()
            // so... basically, nothing more to do here.
        }

        err = releaseBufferLocked(buf, mSlots[buf].mGraphicBuffer, mEglDisplay, EGL_NO_SYNC_KHR);
        if (err < NO_ERROR) {
            GLC_LOGE("releaseTexImage: failed to release buffer: %s (%d)",
                    strerror(-err), err);
            return err;
        }

        if (mReleasedTexImage == NULL) {
            mReleasedTexImage = new EglImage(getDebugTexImageBuffer());
        }

        mCurrentTexture = BufferQueue::INVALID_BUFFER_SLOT;
        mCurrentTextureImage = mReleasedTexImage;
        mCurrentCrop.makeInvalid();
        mCurrentTransform = 0;
        mCurrentTimestamp = 0;
        mCurrentFence = Fence::NO_FENCE;

        if (mAttached) {
            // This binds a dummy buffer (mReleasedTexImage).
            status_t result = bindTextureImageLocked();
            if (result != NO_ERROR) {
                return result;
            }
        } else {
            // detached, don't touch the texture (and we may not even have an
            // EGLDisplay here.
        }
    }

    return NO_ERROR;
}

nsecs_t VDConsumer::getTimestamp() {
    Mutex::Autolock lock(mMutex);
    return mCurrentTimestamp;
}

uint64_t GLConsumer::getFrameNumber() {
    Mutex::Autolock lock(mMutex);
    return mCurrentFrameNumber;
}

sp<GraphicBuffer> VDConsumer::getCurrentBuffer() const {
    Mutex::Autolock lock(mMutex);
    return (mCurrentTextureImage == NULL) ?
            NULL : mCurrentTextureImage->graphicBuffer();
}

status_t VDConsumer::setDefaultBufferSize(uint32_t w, uint32_t h)
{
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        VDC_LOGE("setDefaultBufferSize: VDConsumer is abandoned!");
        return NO_INIT;
    }

    mDefaultWidth = w;
    mDefaultHeight = h;
 
   return mConsumer->setDefaultBufferSize(w, h);
}

void VDConsumer::setName(const String8& name) {
    Mutex::Autolock _l(mMutex);
    if (mAbandoned) {
        VDC_LOGE("setName: VDConsumer is abandoned!");
        return;
    }

    mName = name;
    mConsumer->setConsumerName(name);
}

status_t VDConsumer::setDefaultBufferFormat(PixelFormat defaultFormat) {
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        VDC_LOGE("setDefaultBufferFormat: GLConsumer is abandoned!");
        return NO_INIT;
    }

    return mConsumer->setDefaultBufferFormat(defaultFormat);
}

status_t VDConsumer::setDefaultBufferDataSpace(
        android_dataspace defaultDataSpace) {
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        VDC_LOGE("setDefaultBufferDataSpace: VDConsumer is abandoned!");
        return NO_INIT;
    }

    return mConsumer->setDefaultBufferDataSpace(defaultDataSpace);
}

status_t VDConsumer::setConsumerUsageBits(uint32_t usage) {
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        VDC_LOGE("setConsumerUsageBits: VDConsumer is abandoned!");
        return NO_INIT;
    }

    usage |= DEFAULT_USAGE_FLAGS | GRALLOC_USAGE_SW_READ_OFTEN;

    return mConsumer->setConsumerUsageBits(usage);
}

status_t GLConsumer::setTransformHint(uint32_t hint) {
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        VDC_LOGE("setTransformHint: VDConsumer is abandoned!");
        return NO_INIT;
    }

    return mConsumer->setTransformHint(hint);
}


status_t VDConsumer::setMaxAcquiredBufferCount(int maxAcquiredBuffers) {
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        VDC_LOGE("setMaxAcquiredBufferCount: VDConsumer is abandoned!");
        return NO_INIT;
    }

    return mConsumer->setMaxAcquiredBufferCount(maxAcquiredBuffers);
}

}; // namespace android
