/*
 * Copyright (C) 2020 Rockchip Electronics Co. LTD
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

#include <media/stagefright/foundation/AMessage.h>
#include <inttypes.h>

#include <C2Config.h>
#include <C2Debug.h>
#include <C2PlatformSupport.h>

#include "C2RKComponent.h"
#include "C2RKLogger.h"

namespace android {

C2_LOGGER_ENABLE("C2RKComponent");

std::unique_ptr<C2Work> C2RKComponent::WorkQueue::pop_front() {
    std::unique_ptr<C2Work> work = std::move(mQueue.front().work);
    mQueue.pop_front();
    return work;
}

void C2RKComponent::WorkQueue::push_back(std::unique_ptr<C2Work> work) {
    mQueue.push_back({ std::move(work), NO_DRAIN });
}

bool C2RKComponent::WorkQueue::empty() const {
    return mQueue.empty();
}

void C2RKComponent::WorkQueue::clear() {
    mQueue.clear();
}

uint32_t C2RKComponent::WorkQueue::drainMode() const {
    return mQueue.front().drainMode;
}

void C2RKComponent::WorkQueue::markDrain(uint32_t drainMode) {
    mQueue.push_back({ nullptr, drainMode });
}

////////////////////////////////////////////////////////////////////////////////

C2RKComponent::WorkHandler::WorkHandler() : mRunning(false) {}

void C2RKComponent::WorkHandler::setComponent(
        const std::shared_ptr<C2RKComponent> &thiz) {
    mThiz = thiz;
}

static void Reply(const sp<AMessage> &msg, int32_t *err = nullptr) {
    sp<AReplyToken> replyId;
    CHECK(msg->senderAwaitsResponse(&replyId));
    sp<AMessage> reply = new AMessage;
    if (err) {
        reply->setInt32("err", *err);
    }
    CHECK(reply->postReply(replyId) == OK);
}

void C2RKComponent::WorkHandler::onMessageReceived(const sp<AMessage> &msg) {
    std::shared_ptr<C2RKComponent> thiz = mThiz.lock();
    if (!thiz) {
        Log.I("component not yet set; msg = %s", msg->debugString().c_str());
        sp<AReplyToken> replyId;
        if (msg->senderAwaitsResponse(&replyId)) {
            sp<AMessage> reply = new AMessage;
            reply->setInt32("err", C2_CORRUPTED);

            CHECK(reply->postReply(replyId) == OK);
        }
        return;
    }

    switch (msg->what()) {
        case kWhatProcess: {
            if (mRunning) {
                if (thiz->processQueue()) {
                    CHECK((new AMessage(kWhatProcess, this))->post() == OK);
                }
            } else {
                Log.D("Ignore process message as we're not running");
            }
            break;
        }
        case kWhatInit: {
            int32_t err = thiz->onInit();
            Reply(msg, &err);
            [[fallthrough]];
        }
        case kWhatStart: {
            mRunning = true;
            break;
        }
        case kWhatStop: {
            int32_t err = thiz->onStop();
            thiz->mOutputBlockPool.reset();
            Reply(msg, &err);
            break;
        }
        case kWhatReset: {
            thiz->onReset();
            thiz->mOutputBlockPool.reset();
            mRunning = false;
            Reply(msg);
            break;
        }
        case kWhatRelease: {
            thiz->onRelease();
            thiz->mOutputBlockPool.reset();
            mRunning = false;
            Reply(msg);
            break;
        }
        default: {
            Log.E("Unrecognized msg: %d", msg->what());
            break;
        }
    }
}

class C2RKComponent::BlockingBlockPool : public C2BlockPool {
public:
    BlockingBlockPool(const std::shared_ptr<C2BlockPool>& base): mBase{base} {}

    virtual local_id_t getLocalId() const override {
        return mBase->getLocalId();
    }

    virtual C2Allocator::id_t getAllocatorId() const override {
        return mBase->getAllocatorId();
    }

    virtual c2_status_t fetchLinearBlock(
            uint32_t capacity,
            C2MemoryUsage usage,
            std::shared_ptr<C2LinearBlock>* block) {
        c2_status_t status;
        do {
            status = mBase->fetchLinearBlock(capacity, usage, block);
        } while (status == C2_BLOCKING);
        return status;
    }

    virtual c2_status_t fetchCircularBlock(
            uint32_t capacity,
            C2MemoryUsage usage,
            std::shared_ptr<C2CircularBlock>* block) {
        c2_status_t status;
        do {
            status = mBase->fetchCircularBlock(capacity, usage, block);
        } while (status == C2_BLOCKING);
        return status;
    }

    virtual c2_status_t fetchGraphicBlock(
            uint32_t width, uint32_t height, uint32_t format,
            C2MemoryUsage usage,
            std::shared_ptr<C2GraphicBlock>* block) {
        c2_status_t status;
        do {
            status = mBase->fetchGraphicBlock(width, height, format, usage,
                                              block);
        } while (status == C2_BLOCKING);
        return status;
    }

private:
    std::shared_ptr<C2BlockPool> mBase;
};

////////////////////////////////////////////////////////////////////////////////

namespace {

struct DummyReadView : public C2ReadView {
    DummyReadView() : C2ReadView(C2_NO_INIT) {}
};

}  // namespace

C2RKComponent::C2RKComponent(
        const std::shared_ptr<C2ComponentInterface> &intf)
    : mDummyReadView(DummyReadView()),
      mIntf(intf),
      mLooper(new ALooper),
      mHandler(new WorkHandler) {
    mLooper->setName(intf->getName().c_str());

    CHECK(mLooper->registerHandler(mHandler) > 0);
    CHECK(mLooper->start(false, false, ANDROID_PRIORITY_VIDEO) == OK);
}

C2RKComponent::~C2RKComponent() {
    mLooper->unregisterHandler(mHandler->id());
    CHECK(mLooper->stop() == OK);
}

c2_status_t C2RKComponent::setListener_vb(
        const std::shared_ptr<C2Component::Listener> &listener, c2_blocking_t mayBlock) {
    mHandler->setComponent(shared_from_this());

    Mutexed<ExecState>::Locked state(mExecState);
    if (state->mState == RUNNING) {
        if (listener) {
            return C2_BAD_STATE;
        } else if (!mayBlock) {
            return C2_BLOCKING;
        }
    }
    state->mListener = listener;
    // TODO: wait for listener change to have taken place before returning
    // (e.g. if there is an ongoing listener callback)

    return C2_OK;
}

std::shared_ptr<C2RKComponent> C2RKComponent::sharedFromComponent() {
    return shared_from_this();
}

c2_status_t C2RKComponent::queue_nb(std::list<std::unique_ptr<C2Work>> * const items) {
    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
    }
    bool queueWasEmpty = false;
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queueWasEmpty = queue->empty();
        while (!items->empty()) {
            queue->push_back(std::move(items->front()));
            items->pop_front();
        }
    }
    if (queueWasEmpty) {
        CHECK((new AMessage(WorkHandler::kWhatProcess, mHandler))->post() == OK);
    }

    return C2_OK;
}

c2_status_t C2RKComponent::announce_nb(const std::vector<C2WorkOutline> &items) {
    (void)items;
    return C2_OMITTED;
}

c2_status_t C2RKComponent::flush_sm(
        flush_mode_t flushMode, std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    Log.TraceEnter();
    (void)flushMode;
    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
    }
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);

        // since flush process is time-consuming, set flushing state
        // to discard all work output during process.
        setFlushingState();

        queue->incGeneration();
        // TODO: queue->splicedBy(flushedWork, flushedWork->end());
        while (!queue->empty()) {
            std::unique_ptr<C2Work> work = queue->pop_front();
            if (work) {
                flushedWork->push_back(std::move(work));
            }
        }
        while (!queue->pending().empty()) {
            flushedWork->push_back(std::move(queue->pending().begin()->second));
            std::ignore = queue->pending().erase(queue->pending().begin());
        }
    }

    Log.TraceLeave();
    return C2_OK;
}

c2_status_t C2RKComponent::drain_nb(drain_mode_t drainMode) {
    if (drainMode == DRAIN_CHAIN) {
        return C2_OMITTED;
    }
    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
    }
    bool queueWasEmpty = false;
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queueWasEmpty = queue->empty();
        queue->markDrain(drainMode);
    }
    if (queueWasEmpty) {
        CHECK((new AMessage(WorkHandler::kWhatProcess, mHandler))->post() == OK);
    }
    return C2_OK;
}

c2_status_t C2RKComponent::start() {
    Log.TraceEnter();
    Mutexed<ExecState>::Locked state(mExecState);
    if (state->mState == RUNNING) {
        return C2_BAD_STATE;
    }

    bool needsInit = (state->mState == UNINITIALIZED);
    state.unlock();

    if (needsInit) {
        sp<AMessage> reply;
        sp<AMessage> msg = new AMessage(WorkHandler::kWhatInit, mHandler);
        status_t err = msg->postAndAwaitResponse(&reply);
        if (err == OK && reply != nullptr) {
            CHECK(reply->findInt32("err", &err));
            if (err != OK) {
                return (c2_status_t)err;
            }
        }
    } else {
        CHECK((new AMessage(WorkHandler::kWhatStart, mHandler))->post() == OK);
    }
    state.lock();
    state->mState = RUNNING;

    return C2_OK;
}

c2_status_t C2RKComponent::stop() {
    Log.TraceEnter();

    // since stop process is time-consuming, set flushing state
    // to discard all work output during process.
    setFlushingState();

    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
        state->mState = STOPPED;
    }
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queue->clear();
        queue->pending().clear();
    }
    sp<AMessage> reply;
    status_t err = (new AMessage(
            WorkHandler::kWhatStop, mHandler))->postAndAwaitResponse(&reply);
    if (err == OK && reply != nullptr) {
        CHECK(reply->findInt32("err", &err));
    }

    stopFlushingState();
    return (c2_status_t)err;;
}

c2_status_t C2RKComponent::reset() {
    Log.TraceEnter();

    // since reset process is time-consuming, set flushing state
    // to discard all work output during process.
    setFlushingState();

    {
        Mutexed<ExecState>::Locked state(mExecState);
        state->mState = UNINITIALIZED;
    }
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queue->clear();
        queue->pending().clear();
    }
    sp<AMessage> reply;
    status_t err = (new AMessage(
            WorkHandler::kWhatReset, mHandler))->postAndAwaitResponse(&reply);
    Log.PostErrorIf(err != OK, "postReset");

    stopFlushingState();
    return (c2_status_t)err;
}

c2_status_t C2RKComponent::release() {
    Log.TraceEnter();

    // since release process is time-consuming, set flushing state
    // to discard all work output during process.
    setFlushingState();

    sp<AMessage> reply;
    status_t err = (new AMessage(
            WorkHandler::kWhatRelease, mHandler))->postAndAwaitResponse(&reply);
    Log.PostErrorIf(err != OK, "postRelease");

    stopFlushingState();
    return (c2_status_t)err;
}

std::shared_ptr<C2ComponentInterface> C2RKComponent::intf() {
    return mIntf;
}

namespace {

std::list<std::unique_ptr<C2Work>> vec(std::unique_ptr<C2Work> &work) {
    std::list<std::unique_ptr<C2Work>> ret;
    ret.push_back(std::move(work));
    return ret;
}

}  // namespace

// In flushing state, discard work output
void C2RKComponent::setFlushingState() {
    Mutexed<ExecState>::Locked state(mExecState);
    state->mFlushing = true;
}

void C2RKComponent::stopFlushingState() {
    Mutexed<ExecState>::Locked state(mExecState);
    state->mFlushing = false;
}

bool C2RKComponent::isPendingFlushing() {
    Mutexed<ExecState>::Locked state(mExecState);
    return state->mFlushing;
}

bool C2RKComponent::isPendingWorkExist(uint64_t frameIndex) {
    Mutexed<WorkQueue>::Locked queue(mWorkQueue);
    return (queue->pending().count(frameIndex) > 0);
}

int C2RKComponent::getPendingWorkCountBeforeFrame(uint64_t frameIndex) {
    Mutexed<WorkQueue>::Locked queue(mWorkQueue);
    return std::count_if(
                queue->pending().begin(),  queue->pending().end(),
                [frameIndex](const auto& value) {
                    return (value.second->input.ordinal.frameIndex < frameIndex);
                });
}

void C2RKComponent::finishAllPendingWorks() {
    Mutexed<WorkQueue>::Locked queue(mWorkQueue);
    while (!queue->pending().empty()) {
        std::unique_ptr<C2Work> work = std::move(queue->pending().begin()->second);

        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;

        std::shared_ptr<C2Component::Listener> listener = mExecState.lock()->mListener;
        listener->onWorkDone_nb(shared_from_this(), vec(work));
        Log.D("flush pending work, index %lld", queue->pending().begin()->first);

        std::ignore = queue->pending().erase(queue->pending().begin());
    }
}

void C2RKComponent::finish(
        uint64_t frameIndex,
        std::function<void(const std::unique_ptr<C2Work> &)> fillWork) {

    std::unique_ptr<C2Work> work;

    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);

        // ensure normal output of work with configUpdate
        if (isPendingFlushing()) {
            Log.D("ignore frame output since pending flush");
            return;
        }

        if (queue->pending().count(frameIndex) == 0) {
            Log.W("unknown frame index: %" PRIu64, frameIndex);
            return;
        }
        work = std::move(queue->pending().at(frameIndex));
        std::ignore = queue->pending().erase(frameIndex);
    }

    finish(work, fillWork);
}

void C2RKComponent::finish(
        std::unique_ptr<C2Work> &work,
        std::function<void(const std::unique_ptr<C2Work> &)> fillWork) {
    if (!work) {
        return;
    }

    // ensure normal output of work with configUpdate
    if (isPendingFlushing()) {
        Log.D("ignore frame output since pending flush");
        return;
    }

    fillWork(work);
    std::shared_ptr<C2Component::Listener> listener = mExecState.lock()->mListener;
    listener->onWorkDone_nb(shared_from_this(), vec(work));
    Log.D("returning pending work");
}

void C2RKComponent::cloneAndSend(
        uint64_t frameIndex,
        const std::unique_ptr<C2Work> &currentWork,
        std::function<void(const std::unique_ptr<C2Work> &)> fillWork) {
    std::unique_ptr<C2Work> work(new C2Work);
    if (currentWork->input.ordinal.frameIndex == frameIndex) {
        work->input.flags = currentWork->input.flags;
        work->input.ordinal = currentWork->input.ordinal;
    } else {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        if (queue->pending().count(frameIndex) == 0) {
            Log.W("unknown frame index: %" PRIu64, frameIndex);
            return;
        }
        work->input.flags = queue->pending().at(frameIndex)->input.flags;
        work->input.ordinal = queue->pending().at(frameIndex)->input.ordinal;
    }
    std::ignore = work->worklets.emplace_back(new C2Worklet);
    if (work) {
        fillWork(work);
        std::shared_ptr<C2Component::Listener> listener = mExecState.lock()->mListener;
        listener->onWorkDone_nb(shared_from_this(), vec(work));
        Log.D("cloned and sending work");
    }
}

bool C2RKComponent::processQueue() {
    std::unique_ptr<C2Work> work;
    uint64_t generation;
    int32_t drainMode;
    bool isFlushPending = false;
    bool hasQueuedWork = false;
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        if (queue->empty()) {
            return false;
        }

        generation = queue->generation();
        drainMode = queue->drainMode();
        isFlushPending = queue->popPendingFlush();
        work = queue->pop_front();
        hasQueuedWork = !queue->empty();
    }
    if (isFlushPending) {
        Log.D("processing pending flush");
        c2_status_t err = onFlush_sm();
        if (err != C2_OK) {
            Log.E("flush err: %d", err);
            // TODO: error
        }

        stopFlushingState();
    }

    if (!mOutputBlockPool) {
        c2_status_t err = [this] {
            // TODO: don't use query_vb
            C2StreamBufferTypeSetting::output outputFormat(0u);
            std::vector<std::unique_ptr<C2Param>> params;
            c2_status_t err = intf()->query_vb(
                    { &outputFormat },
                    { C2PortBlockPoolsTuning::output::PARAM_TYPE },
                    C2_DONT_BLOCK,
                    &params);
            if (err != C2_OK && err != C2_BAD_INDEX) {
                Log.E("query err = %d", err);
                return err;
            }
            C2BlockPool::local_id_t poolId =
                outputFormat.value == C2BufferData::GRAPHIC
                        ? C2BlockPool::BASIC_GRAPHIC
                        : C2BlockPool::BASIC_LINEAR;
            if (params.size()) {
                C2PortBlockPoolsTuning::output *outputPools =
                    C2PortBlockPoolsTuning::output::From(params[0].get());
                if (outputPools && outputPools->flexCount() >= 1) {
                    poolId = outputPools->m.values[0];
                }
            }

            std::shared_ptr<C2BlockPool> blockPool;
            err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
            Log.D("Using output block pool with poolID %llu => got %llu - %d",
                   (unsigned long long)poolId,
                   (unsigned long long)(blockPool ? blockPool->getLocalId() : 111000111), err);
            if (err == C2_OK) {
                mOutputBlockPool = std::make_shared<BlockingBlockPool>(blockPool);
            }
            return err;
        }();
        if (err != C2_OK) {
            Mutexed<ExecState>::Locked state(mExecState);
            std::shared_ptr<C2Component::Listener> listener = state->mListener;
            state.unlock();
            listener->onError_nb(shared_from_this(), err);
            return hasQueuedWork;
        }
    }

    if (!work) {
        c2_status_t err = drain(drainMode, mOutputBlockPool);
        if (err != C2_OK) {
            Mutexed<ExecState>::Locked state(mExecState);
            std::shared_ptr<C2Component::Listener> listener = state->mListener;
            state.unlock();
            listener->onError_nb(shared_from_this(), err);
        }
        return hasQueuedWork;
    }

    {
        std::vector<C2Param *> updates;
        for (const std::unique_ptr<C2Param> &param: work->input.configUpdate) {
            if (param) {
                std::ignore = updates.emplace_back(param.get());
            }
        }
        if (!updates.empty()) {
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            c2_status_t err = intf()->config_vb(updates, C2_MAY_BLOCK, &failures);
            Log.D("applied %zu configUpdates => %s (%d)", updates.size(), asString(err), err);
        }
    }

    Log.D("start processing frame #%" PRIu64, work->input.ordinal.frameIndex.peeku());
    // If input buffer list is not empty, it means we have some input to process on.
    // However, input could be a null buffer. In such case, clear the buffer list
    // before making call to process().
    if (!work->input.buffers.empty() && !work->input.buffers[0]) {
        Log.I("Encountered null input buffer. Clearing the input buffer");
        work->input.buffers.clear();
    }
    process(work, mOutputBlockPool);
    Log.D("processed frame #%" PRIu64, work->input.ordinal.frameIndex.peeku());
    Mutexed<WorkQueue>::Locked queue(mWorkQueue);
    if (queue->generation() != generation) {
        Log.I("work form old generation: %" PRIu64 " now %" PRIu64, queue->generation(), generation);
        work->result = C2_NOT_FOUND;
        queue.unlock();

        Mutexed<ExecState>::Locked state(mExecState);
        std::shared_ptr<C2Component::Listener> listener = state->mListener;
        state.unlock();
        listener->onWorkDone_nb(shared_from_this(), vec(work));
        return hasQueuedWork;
    }
    if (work->workletsProcessed != 0u) {
        queue.unlock();
        Mutexed<ExecState>::Locked state(mExecState);
        Log.D("returning this work");
        std::shared_ptr<C2Component::Listener> listener = state->mListener;
        state.unlock();
        listener->onWorkDone_nb(shared_from_this(), vec(work));
    } else {
        Log.D("queue pending work");
        work->input.buffers.clear();
        std::unique_ptr<C2Work> unexpected;

        uint64_t frameIndex = work->input.ordinal.frameIndex.peeku();
        if (queue->pending().count(frameIndex) != 0) {
            unexpected = std::move(queue->pending().at(frameIndex));
            std::ignore = queue->pending().erase(frameIndex);
        }
        std::ignore = queue->pending().emplace(frameIndex, std::move(work));

        queue.unlock();
        if (unexpected) {
            Log.I("unexpected pending work");
            unexpected->result = C2_CORRUPTED;
            Mutexed<ExecState>::Locked state(mExecState);
            std::shared_ptr<C2Component::Listener> listener = state->mListener;
            state.unlock();
            listener->onWorkDone_nb(shared_from_this(), vec(unexpected));
        }
    }
    return hasQueuedWork;
}

std::shared_ptr<C2Buffer> C2RKComponent::createLinearBuffer(
        const std::shared_ptr<C2LinearBlock> &block) {
    return createLinearBuffer(block, block->offset(), block->size());
}

std::shared_ptr<C2Buffer> C2RKComponent::createLinearBuffer(
        const std::shared_ptr<C2LinearBlock> &block, size_t offset, size_t size) {
    return C2Buffer::CreateLinearBuffer(block->share(offset, size, ::C2Fence()));
}

std::shared_ptr<C2Buffer> C2RKComponent::createGraphicBuffer(
        const std::shared_ptr<C2GraphicBlock> &block) {
    return createGraphicBuffer(block, C2Rect(block->width(), block->height()));
}

std::shared_ptr<C2Buffer> C2RKComponent::createGraphicBuffer(
        const std::shared_ptr<C2GraphicBlock> &block, const C2Rect &crop) {
    return C2Buffer::CreateGraphicBuffer(block->share(crop, ::C2Fence()));
}

} // namespace android
