/*
 * Copyright (C) 2022 The Android Open Source Project
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

#define LOG_TAG "AidlBufferPoolStatus"
//#define LOG_NDEBUG 0

#include <thread>
#include <time.h>
#include <aidl/android/hardware/media/bufferpool2/BufferStatus.h>
#include "BufferStatus.h"

namespace aidl::android::hardware::media::bufferpool2::implementation {

using aidl::android::hardware::media::bufferpool2::BufferStatus;

bool isMessageLater(uint32_t curMsgId, uint32_t prevMsgId) {
    return curMsgId != prevMsgId && curMsgId - prevMsgId < prevMsgId - curMsgId;
}

bool isBufferInRange(BufferId from, BufferId to, BufferId bufferId) {
    if (from < to) {
        return from <= bufferId && bufferId < to;
    } else { // wrap happens
        return from <= bufferId || bufferId < to;
    }
}

static constexpr int kNumElementsInQueue = 1024*16;
static constexpr int kMinElementsToSyncInQueue = 128;

BufferPoolStatus BufferStatusObserver::open(
        ConnectionId id, StatusDescriptor* fmqDescPtr) {
    if (mBufferStatusQueues.find(id) != mBufferStatusQueues.end()) {
        ALOGE("connection id collision %lld", (unsigned long long)id);
        return ResultStatus::CRITICAL_ERROR;
    }
    auto queue = std::make_unique<BufferStatusQueue>(kNumElementsInQueue);
    if (!queue || queue->isValid() == false) {
        return ResultStatus::NO_MEMORY;
    }
    *fmqDescPtr = queue->dupeDesc();
    auto result = mBufferStatusQueues.insert(
            std::make_pair(id, std::move(queue)));
    if (!result.second) {
        return ResultStatus::NO_MEMORY;
    }
    return ResultStatus::OK;
}

BufferPoolStatus BufferStatusObserver::close(ConnectionId id) {
    if (mBufferStatusQueues.find(id) == mBufferStatusQueues.end()) {
        return ResultStatus::CRITICAL_ERROR;
    }
    mBufferStatusQueues.erase(id);
    return ResultStatus::OK;
}

void BufferStatusObserver::getBufferStatusChanges(std::vector<BufferStatusMessage> &messages) {
    for (auto it = mBufferStatusQueues.begin(); it != mBufferStatusQueues.end(); ++it) {
        BufferStatusMessage message;
        size_t avail = it->second->availableToRead();
        while (avail > 0) {
            if (!it->second->read(&message, 1)) {
                // Since available # of reads are already confirmed,
                // this should not happen.
                // TODO: error handling (spurious client?)
                ALOGW("FMQ message cannot be read from %lld", (long long)it->first);
                return;
            }
            message.connectionId = it->first;
            messages.push_back(message);
            --avail;
        }
    }
}

BufferStatusChannel::BufferStatusChannel(
        const StatusDescriptor &fmqDesc) {
    auto queue = std::make_unique<BufferStatusQueue>(fmqDesc);
    if (!queue || queue->isValid() == false) {
        mValid = false;
        return;
    }
    mValid  = true;
    mBufferStatusQueue = std::move(queue);
}

bool BufferStatusChannel::isValid() {
    return mValid;
}

bool BufferStatusChannel::needsSync() {
    if (mValid) {
        size_t avail = mBufferStatusQueue->availableToWrite();
        return avail + kMinElementsToSyncInQueue < kNumElementsInQueue;
    }
    return false;
}

void BufferStatusChannel::postBufferRelease(
        ConnectionId connectionId,
        std::list<BufferId> &pending, std::list<BufferId> &posted) {
    if (mValid && pending.size() > 0) {
        size_t avail = mBufferStatusQueue->availableToWrite();
        avail = std::min(avail, pending.size());
        BufferStatusMessage message;
        for (size_t i = 0 ; i < avail; ++i) {
            BufferId id = pending.front();
            message.status = BufferStatus::NOT_USED;
            message.bufferId = id;
            message.connectionId = connectionId;
            if (!mBufferStatusQueue->write(&message, 1)) {
                // Since available # of writes are already confirmed,
                // this should not happen.
                // TODO: error handing?
                ALOGW("FMQ message cannot be sent from %lld", (long long)connectionId);
                return;
            }
            pending.pop_front();
            posted.push_back(id);
        }
    }
}

void BufferStatusChannel::postBufferInvalidateAck(
        ConnectionId connectionId,
        uint32_t invalidateId,
        bool *invalidated) {
    if (mValid && !*invalidated) {
        size_t avail = mBufferStatusQueue->availableToWrite();
        if (avail > 0) {
            BufferStatusMessage message;
            message.status = BufferStatus::INVALIDATION_ACK;
            message.bufferId = invalidateId;
            message.connectionId = connectionId;
            if (!mBufferStatusQueue->write(&message, 1)) {
                // Since available # of writes are already confirmed,
                // this should not happen.
                // TODO: error handing?
                ALOGW("FMQ message cannot be sent from %lld", (long long)connectionId);
                return;
            }
            *invalidated = true;
        }
    }
}

bool BufferStatusChannel::postBufferStatusMessage(
        TransactionId transactionId, BufferId bufferId,
        BufferStatus status, ConnectionId connectionId, ConnectionId targetId,
        std::list<BufferId> &pending, std::list<BufferId> &posted) {
    if (mValid) {
        size_t avail = mBufferStatusQueue->availableToWrite();
        size_t numPending = pending.size();
        if (avail >= numPending + 1) {
            BufferStatusMessage release, message;
            for (size_t i = 0; i < numPending; ++i) {
                BufferId id = pending.front();
                release.status = BufferStatus::NOT_USED;
                release.bufferId = id;
                release.connectionId = connectionId;
                if (!mBufferStatusQueue->write(&release, 1)) {
                    // Since available # of writes are already confirmed,
                    // this should not happen.
                    // TODO: error handling?
                    ALOGW("FMQ message cannot be sent from %lld", (long long)connectionId);
                    return false;
                }
                pending.pop_front();
                posted.push_back(id);
            }
            message.transactionId = transactionId;
            message.bufferId = bufferId;
            message.status = status;
            message.connectionId = connectionId;
            message.targetConnectionId = targetId;
            // TODO : timesatamp
            message.timestampUs = 0;
            if (!mBufferStatusQueue->write(&message, 1)) {
                // Since available # of writes are already confirmed,
                // this should not happen.
                ALOGW("FMQ message cannot be sent from %lld", (long long)connectionId);
                return false;
            }
            return true;
        }
    }
    return false;
}

BufferInvalidationListener::BufferInvalidationListener(
        const InvalidationDescriptor &fmqDesc) {
    std::unique_ptr<BufferInvalidationQueue> queue =
            std::make_unique<BufferInvalidationQueue>(fmqDesc);
    if (!queue || queue->isValid() == false) {
        mValid = false;
        return;
    }
    mValid  = true;
    mBufferInvalidationQueue = std::move(queue);
    // drain previous messages
    size_t avail = std::min(
            mBufferInvalidationQueue->availableToRead(), (size_t) kNumElementsInQueue);
    std::vector<BufferInvalidationMessage> temp(avail);
    if (avail > 0) {
        mBufferInvalidationQueue->read(temp.data(), avail);
    }
}

void BufferInvalidationListener::getInvalidations(
        std::vector<BufferInvalidationMessage> &messages) {
    // Try twice in case of overflow.
    // TODO: handling overflow though it may not happen.
    for (int i = 0; i < 2; ++i) {
        size_t avail = std::min(
                mBufferInvalidationQueue->availableToRead(), (size_t) kNumElementsInQueue);
        if (avail > 0) {
            std::vector<BufferInvalidationMessage> temp(avail);
            if (mBufferInvalidationQueue->read(temp.data(), avail)) {
                messages.reserve(messages.size() + avail);
                for (auto it = temp.begin(); it != temp.end(); ++it) {
                    messages.push_back(*it);
                }
                break;
            }
        } else {
            return;
        }
    }
}

bool BufferInvalidationListener::isValid() {
    return mValid;
}

BufferInvalidationChannel::BufferInvalidationChannel()
    : mValid(true),
      mBufferInvalidationQueue(
              std::make_unique<BufferInvalidationQueue>(kNumElementsInQueue, true)) {
    if (!mBufferInvalidationQueue || mBufferInvalidationQueue->isValid() == false) {
        mValid = false;
    }
}

bool BufferInvalidationChannel::isValid() {
    return mValid;
}

void BufferInvalidationChannel::getDesc(InvalidationDescriptor *fmqDescPtr) {
    if (mValid) {
        *fmqDescPtr = mBufferInvalidationQueue->dupeDesc();
    }
    // TODO: writing invalid descriptor?
}

void BufferInvalidationChannel::postInvalidation(
        uint32_t msgId, BufferId fromId, BufferId toId) {
    BufferInvalidationMessage message;

    message.messageId = msgId;
    message.fromBufferId = fromId;
    message.toBufferId = toId;
    // TODO: handle failure (it does not happen normally.)
    mBufferInvalidationQueue->write(&message);
}

}  // namespace ::aidl::android::hardware::media::bufferpool2::implementation

