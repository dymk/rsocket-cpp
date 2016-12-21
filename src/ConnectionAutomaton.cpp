// Copyright 2004-present Facebook. All Rights Reserved.

#include "src/ConnectionAutomaton.h"

#include <folly/ExceptionWrapper.h>
#include <folly/String.h>
#include "src/AbstractStreamAutomaton.h"
#include "src/ClientResumeStatusCallback.h"
#include "src/DuplexConnection.h"
#include "src/FrameTransport.h"
#include "src/Stats.h"
#include "src/StreamState.h"

namespace reactivesocket {

ConnectionAutomaton::ConnectionAutomaton(
    StreamAutomatonFactory factory,
    std::shared_ptr<StreamState> streamState,
    ResumeListener resumeListener,
    Stats& stats,
    const std::shared_ptr<KeepaliveTimer>& keepaliveTimer,
    bool isServer,
    bool isResumable,
    std::function<void()> onConnected,
    std::function<void()> onDisconnected,
    std::function<void()> onClosed)
    : factory_(std::move(factory)),
      streamState_(std::move(streamState)),
      stats_(stats),
      isServer_(isServer),
      isResumable_(isResumable),
      onConnected_(std::move(onConnected)),
      onDisconnected_(std::move(onDisconnected)),
      onClosed_(std::move(onClosed)),
      resumeListener_(resumeListener),
      keepaliveTimer_(keepaliveTimer) {
  // We deliberately do not "open" input or output to avoid having c'tor on the
  // stack when processing any signals from the connection. See ::connect and
  // ::onSubscribe.
  CHECK(streamState_);
  CHECK(onConnected_);
  CHECK(onDisconnected_);
  CHECK(onClosed_);
}

ConnectionAutomaton::~ConnectionAutomaton() {
  VLOG(6) << "~ConnectionAutomaton";
  // We rely on SubscriptionPtr and SubscriberPtr to dispatch appropriate
  // terminal signals.
  DCHECK(!resumeCallback_);
  DCHECK(
      !frameTransport_); // the instance should be closed by via close() method
}

void ConnectionAutomaton::setResumable(bool resumable) {
  DCHECK(!frameTransport_); // we allow to set this flag before we are connected
  isResumable_ = resumable;
}

void ConnectionAutomaton::connect(
    std::shared_ptr<FrameTransport> frameTransport) {
  CHECK(!frameTransport_);
  CHECK(frameTransport);
  CHECK(!frameTransport->isClosed());

  frameTransport_ = std::move(frameTransport);
  // We need to create a hard reference to frameTransport_ to make sure the
  // instance survives until the setFrameProcessor returns. There can be
  // terminating signals processed in that call which will nullify
  // frameTransport_
  auto frameTransportCopy = frameTransport_;
  frameTransport_->setFrameProcessor(shared_from_this());

  // setFrameProcessor starts pulling frames from duplex connection
  // the connection may also get closed before the method returns so
  // we need to check again
  auto outputFrames = streamState_->moveOutputFrames();
  if (frameTransport_) {
    for (auto& frame : outputFrames) {
      outputFrame(std::move(frame));
    }
    onConnected_();
  } else {
    LOG_IF(WARNING, !outputFrames.empty()) << "transport closed, throwing away "
                                           << outputFrames.size() << " frames.";
  }

  // TODO: move to appropriate place
  stats_.socketCreated();
}

void ConnectionAutomaton::disconnect() {
  VLOG(6) << "disconnect";
  if (!frameTransport_) {
    return;
  }

  closeFrameTransport(folly::exception_wrapper());
  stats_.socketDisconnected();
  onDisconnected_();
}

void ConnectionAutomaton::close() {
  close(folly::exception_wrapper(), StreamCompletionSignal::SOCKET_CLOSED);
}

void ConnectionAutomaton::close(
    folly::exception_wrapper ex,
    StreamCompletionSignal signal) {
  VLOG(6) << "close";
  closeStreams(signal);
  closeFrameTransport(std::move(ex));
  if (onClosed_) {
    stats_.socketClosed();
    auto onClosed = std::move(onClosed_);
    onClosed_ = nullptr;
    onClosed();
  }
}

void ConnectionAutomaton::closeFrameTransport(folly::exception_wrapper ex) {
  if (!frameTransport_) {
    DCHECK(!resumeCallback_);
    return;
  }

  if (resumeCallback_) {
    resumeCallback_->onConnectionError(
        std::runtime_error(ex ? ex.what().c_str() : "connection closing"));
    resumeCallback_.reset();
  }

  frameTransport_->close(std::move(ex));
  frameTransport_ = nullptr;
}

void ConnectionAutomaton::closeWithError(Frame_ERROR&& error) {
  VLOG(4) << "closeWithError "
          << error.payload_.data->cloneAsValue().moveToFbString();

  outputFrameOrEnqueue(error.serializeOut());
  close(folly::exception_wrapper(), StreamCompletionSignal::ERROR);
}

void ConnectionAutomaton::reconnect(
    std::shared_ptr<FrameTransport> newFrameTransport,
    std::unique_ptr<ClientResumeStatusCallback> resumeCallback) {
  CHECK(newFrameTransport);
  CHECK(resumeCallback);
  CHECK(!resumeCallback_);
  CHECK(isResumable_);
  CHECK(!isServer_);

  disconnect();
  // TODO: output frame buffer should not be written to the new connection until
  // we receive resume ok
  resumeCallback_ = std::move(resumeCallback);
  connect(std::move(newFrameTransport));
}

void ConnectionAutomaton::addStream(
    StreamId streamId,
    std::shared_ptr<AbstractStreamAutomaton> automaton) {
  auto result = streamState_->streams_.emplace(streamId, std::move(automaton));
  (void)result;
  assert(result.second);
}

void ConnectionAutomaton::endStream(
    StreamId streamId,
    StreamCompletionSignal signal) {
  VLOG(6) << "endStream";
  // The signal must be idempotent.
  if (!endStreamInternal(streamId, signal)) {
    return;
  }
  // TODO(stupaq): handle connection-level errors
  assert(
      signal == StreamCompletionSignal::GRACEFUL ||
      signal == StreamCompletionSignal::ERROR);
}

bool ConnectionAutomaton::endStreamInternal(
    StreamId streamId,
    StreamCompletionSignal signal) {
  VLOG(6) << "endStreamInternal";
  auto it = streamState_->streams_.find(streamId);
  if (it == streamState_->streams_.end()) {
    // Unsubscribe handshake initiated by the connection, we're done.
    return false;
  }
  // Remove from the map before notifying the automaton.
  auto automaton = std::move(it->second);
  streamState_->streams_.erase(it);
  automaton->endStream(signal);
  return true;
}

void ConnectionAutomaton::closeStreams(StreamCompletionSignal signal) {
  // Close all streams.
  while (!streamState_->streams_.empty()) {
    auto oldSize = streamState_->streams_.size();
    auto result =
        endStreamInternal(streamState_->streams_.begin()->first, signal);
    (void)oldSize;
    (void)result;
    // TODO(stupaq): what kind of a user action could violate these
    // assertions?
    assert(result);
    assert(streamState_->streams_.size() == oldSize - 1);
  }
}

void ConnectionAutomaton::processFrame(std::unique_ptr<folly::IOBuf> frame) {
  auto frameType = FrameHeader::peekType(*frame);

  std::stringstream ss;
  ss << frameType;

  stats_.frameRead(ss.str());

  // TODO(tmont): If a frame is invalid, it will still be tracked. However, we
  // actually want that. We want to keep
  // each side in sync, even if a frame is invalid.
  streamState_->resumeTracker_->trackReceivedFrame(*frame);

  auto streamIdPtr = FrameHeader::peekStreamId(*frame);
  if (!streamIdPtr) {
    // Failed to deserialize the frame.
    closeWithError(Frame_ERROR::connectionError("invalid frame"));
    return;
  }
  auto streamId = *streamIdPtr;
  if (streamId == 0) {
    onConnectionFrame(std::move(frame));
    return;
  }
  auto it = streamState_->streams_.find(streamId);
  if (it == streamState_->streams_.end()) {
    handleUnknownStream(streamId, std::move(frame));
    return;
  }
  auto automaton = it->second;
  // Can deliver the frame.
  automaton->onNextFrame(std::move(frame));
}

void ConnectionAutomaton::onTerminal(
    folly::exception_wrapper ex,
    StreamCompletionSignal signal) {
  if (isResumable_) {
    disconnect();
  } else {
    close(std::move(ex), signal);
  }
}

void ConnectionAutomaton::onConnectionFrame(
    std::unique_ptr<folly::IOBuf> payload) {
  auto type = FrameHeader::peekType(*payload);
  switch (type) {
    case FrameType::KEEPALIVE: {
      Frame_KEEPALIVE frame;
      if (!deserializeFrameOrError(frame, std::move(payload))) {
        return;
      }
      if (isServer_) {
        if (frame.header_.flags_ & FrameFlags_KEEPALIVE_RESPOND) {
          frame.header_.flags_ &= ~(FrameFlags_KEEPALIVE_RESPOND);
          outputFrameOrEnqueue(frame.serializeOut());
        } else {
          closeWithError(
              Frame_ERROR::connectionError("keepalive without flag"));
        }

        streamState_->resumeCache_->resetUpToPosition(frame.position_);
      } else {
        if (frame.header_.flags_ & FrameFlags_KEEPALIVE_RESPOND) {
          closeWithError(Frame_ERROR::connectionError(
              "client received keepalive with respond flag"));
        } else if (keepaliveTimer_) {
          keepaliveTimer_->keepaliveReceived();
        }
      }
      return;
    }
    case FrameType::SETUP: {
      // TODO(tmont): check for ENABLE_RESUME and make sure isResumable_ is true
      factory_(*this, 0, std::move(payload));
      return;
    }
    case FrameType::METADATA_PUSH: {
      factory_(*this, 0, std::move(payload));
      return;
    }
    case FrameType::RESUME: {
      Frame_RESUME frame;
      if (!deserializeFrameOrError(frame, std::move(payload))) {
        return;
      }
      bool canResume = false;

      if (isServer_ && isResumable_) {
        auto streamState = resumeListener_(frame.token_);
        if (nullptr != streamState) {
          canResume = true;
          useStreamState(streamState);
        }
      }

      if (canResume) {
        outputFrameOrEnqueue(
            Frame_RESUME_OK(streamState_->resumeTracker_->impliedPosition())
                .serializeOut());
        for (auto it : streamState_->streams_) {
          const StreamId streamId = it.first;

          if (streamState_->resumeCache_->isPositionAvailable(
                  frame.position_, streamId)) {
            it.second->onCleanResume();
          } else {
            it.second->onDirtyResume();
          }
        }
      } else {
        closeWithError(Frame_ERROR::connectionError("can not resume"));
      }
      return;
    }
    case FrameType::RESUME_OK: {
      Frame_RESUME_OK frame;
      if (!deserializeFrameOrError(frame, std::move(payload))) {
        return;
      }
      if (resumeCallback_) {
        if (!isServer_ && isResumable_ &&
            streamState_->resumeCache_->isPositionAvailable(frame.position_)) {
          // TODO(lehecka): finish stuff here
          resumeCallback_->onResumeOk();
          resumeCallback_.reset();
        } else {
          closeWithError(Frame_ERROR::connectionError("can not resume"));
        }
      } else {
        // TODO: this will be handled in the new way in a different automaton
        closeWithError(Frame_ERROR::unexpectedFrame());
      }
      return;
    }
    case FrameType::ERROR: {
      Frame_ERROR frame;
      if (!deserializeFrameOrError(frame, std::move(payload))) {
        return;
      }

      // TODO: handle INVALID_SETUP, UNSUPPORTED_SETUP, REJECTED_SETUP

      if (frame.errorCode_ == ErrorCode::CONNECTION_ERROR && resumeCallback_) {
        resumeCallback_->onResumeError(
            std::runtime_error(frame.payload_.moveDataToString()));
        resumeCallback_.reset();
        // fall through
      }
      return;
    }
    default:
      closeWithError(Frame_ERROR::unexpectedFrame());
      return;
  }
}

void ConnectionAutomaton::handleUnknownStream(
    StreamId streamId,
    std::unique_ptr<folly::IOBuf> payload) {
  // TODO(stupaq): there are some rules about monotonically increasing stream
  // IDs -- let's forget about them for a moment
  factory_(*this, streamId, std::move(payload));
}
/// @}

void ConnectionAutomaton::sendKeepalive() {
  Frame_KEEPALIVE pingFrame(
      FrameFlags_KEEPALIVE_RESPOND,
      streamState_->resumeTracker_->impliedPosition(),
      folly::IOBuf::create(0));
  outputFrameOrEnqueue(pingFrame.serializeOut());
}

void ConnectionAutomaton::sendResume(const ResumeIdentificationToken& token) {
  Frame_RESUME resumeFrame(
      token, streamState_->resumeTracker_->impliedPosition());
  outputFrameOrEnqueue(resumeFrame.serializeOut());
}

bool ConnectionAutomaton::isPositionAvailable(ResumePosition position) {
  return streamState_->resumeCache_->isPositionAvailable(position);
}

ResumePosition ConnectionAutomaton::positionDifference(
    ResumePosition position) {
  return streamState_->resumeCache_->position() - position;
}

void ConnectionAutomaton::outputFrameOrEnqueue(
    std::unique_ptr<folly::IOBuf> frame) {
  if (frameTransport_) {
    outputFrame(std::move(frame));
  } else {
    streamState_->enqueueOutputFrame(std::move(frame));
  }
}

void ConnectionAutomaton::outputFrame(std::unique_ptr<folly::IOBuf> frame) {
  DCHECK(frameTransport_);

  std::stringstream ss;
  ss << FrameHeader::peekType(*frame);
  stats_.frameWritten(ss.str());

  streamState_->resumeCache_->trackSentFrame(*frame);
  frameTransport_->outputFrameOrEnqueue(std::move(frame));
}

void ConnectionAutomaton::useStreamState(
    std::shared_ptr<StreamState> streamState) {
  CHECK(streamState);
  if (isServer_ && isResumable_) {
    streamState_.swap(streamState);
  }
}
}
