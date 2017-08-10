// Copyright 2004-present Facebook. All Rights Reserved.

#include "rsocket/statemachine/RSocketStateMachine.h"

#include <folly/ExceptionWrapper.h>
#include <folly/Format.h>
#include <folly/Optional.h>
#include <folly/String.h>
#include <folly/io/async/EventBase.h>

#include "rsocket/DuplexConnection.h"
#include "rsocket/RSocketConnectionEvents.h"
#include "rsocket/RSocketParameters.h"
#include "rsocket/RSocketResponder.h"
#include "rsocket/RSocketStats.h"
#include "rsocket/framing/Frame.h"
#include "rsocket/framing/FrameSerializer.h"
#include "rsocket/framing/FrameTransport.h"
#include "rsocket/internal/ClientResumeStatusCallback.h"
#include "rsocket/internal/InMemResumeManager.h"
#include "rsocket/statemachine/ChannelResponder.h"
#include "rsocket/statemachine/StreamState.h"
#include "rsocket/statemachine/StreamStateMachineBase.h"

namespace rsocket {

RSocketStateMachine::RSocketStateMachine(
    folly::Executor& executor,
    std::shared_ptr<RSocketResponder> requestResponder,
    std::unique_ptr<KeepaliveTimer> keepaliveTimer,
    ReactiveSocketMode mode,
    std::shared_ptr<RSocketStats> stats,
    std::shared_ptr<RSocketConnectionEvents> connectionEvents,
    std::shared_ptr<ResumeManager> resumeManager)
    : mode_(mode),
      stats_(stats ? stats : RSocketStats::noop()),
      streamState_(*stats_),
      resumeManager_(resumeManager ? resumeManager : std::make_shared<InMemResumeManager>(stats_)),
      requestResponder_(std::move(requestResponder)),
      keepaliveTimer_(std::move(keepaliveTimer)),
      streamsFactory_(*this, mode),
      connectionEvents_(connectionEvents),
      executor_(executor) {
  // We deliberately do not "open" input or output to avoid having c'tor on the
  // stack when processing any signals from the connection. See ::connect and
  // ::onSubscribe.

  CHECK(requestResponder_);

  stats_->socketCreated();
}

RSocketStateMachine::~RSocketStateMachine() {
  // this destructor can be called from a different thread because the stream
  // automatons destroyed on different threads can be the last ones referencing
  // this.

  VLOG(3) << "~RSocketStateMachine";
  // We rely on SubscriptionPtr and SubscriberPtr to dispatch appropriate
  // terminal signals.
  DCHECK(!resumeCallback_);
  DCHECK(isDisconnectedOrClosed()); // the instance should be closed by via
  // close method
}

void RSocketStateMachine::setResumable(bool resumable) {
  debugCheckCorrectExecutor();
  // We should set this flag before we are connected
  DCHECK(isDisconnectedOrClosed());
  remoteResumeable_ = isResumable_ = resumable;
}

bool RSocketStateMachine::connectServer(
    yarpl::Reference<FrameTransport> frameTransport,
    const SetupParameters& setupParams) {
  setResumable(setupParams.resumable);
  return connect(std::move(frameTransport), true, setupParams.protocolVersion);
}

bool RSocketStateMachine::resumeServer(
    yarpl::Reference<FrameTransport> frameTransport,
    const ResumeParameters& resumeParams) {
  return connect(
             std::move(frameTransport), false, resumeParams.protocolVersion) &&
      resumeFromPositionOrClose(
             resumeParams.serverPosition, resumeParams.clientPosition);
}

bool RSocketStateMachine::connect(
    yarpl::Reference<FrameTransport> frameTransport,
    bool sendingPendingFrames,
    ProtocolVersion protocolVersion) {
  debugCheckCorrectExecutor();
  CHECK(isDisconnectedOrClosed());
  CHECK(frameTransport);
  CHECK(!frameTransport->isClosed());
  if (protocolVersion != ProtocolVersion::Unknown) {
    if (frameSerializer_) {
      if (frameSerializer_->protocolVersion() != protocolVersion) {
        DCHECK(false);
        std::runtime_error exn("Protocol version mismatch");
        frameTransport->closeWithError(std::move(exn));
        return false;
      }
    } else {
      frameSerializer_ =
          FrameSerializer::createFrameSerializer(protocolVersion);
      if (!frameSerializer_) {
        DCHECK(false);
        std::runtime_error exn("Invalid protocol version");
        frameTransport->closeWithError(std::move(exn));
        return false;
      }
    }
  }

  frameTransport_ = std::move(frameTransport);

  if (connectionEvents_) {
    connectionEvents_->onConnected();
  }

  // We need to create a hard reference to frameTransport_ to make sure the
  // instance survives until the setFrameProcessor returns.  There can be
  // terminating signals processed in that call which will nullify
  // frameTransport_.
  auto frameTransportCopy = frameTransport_;

  // Keep a reference to this, as processing frames might close the
  // ReactiveSocket instance.
  auto copyThis = shared_from_this();
  frameTransport_->setFrameProcessor(copyThis);

  if (sendingPendingFrames) {
    DCHECK(!resumeCallback_);
    // we are free to try to send frames again
    // not all frames might be sent if the connection breaks, the rest of them
    // will queue up again
    auto outputFrames = streamState_.moveOutputPendingFrames();
    for (auto& frame : outputFrames) {
      outputFrameOrEnqueue(std::move(frame));
    }

    // TODO: turn on only after setup frame was received
    if (keepaliveTimer_) {
      keepaliveTimer_->start(shared_from_this());
    }
  }

  return true;
}

void RSocketStateMachine::disconnect(folly::exception_wrapper ex) {
  debugCheckCorrectExecutor();
  VLOG(6) << "disconnect";
  if (isDisconnectedOrClosed()) {
    return;
  }

  if (connectionEvents_) {
    connectionEvents_->onDisconnected(ex);
  }

  closeFrameTransport(std::move(ex), StreamCompletionSignal::CONNECTION_END);

  if (connectionEvents_) {
    connectionEvents_->onStreamsPaused();
  }

  stats_->socketDisconnected();
}

void RSocketStateMachine::close(
    folly::exception_wrapper ex,
    StreamCompletionSignal signal) {
  debugCheckCorrectExecutor();

  if (isClosed_) {
    return;
  }
  isClosed_ = true;
  stats_->socketClosed(signal);

  VLOG(6) << "close";

  if (resumeCallback_) {
    resumeCallback_->onResumeError(
        ConnectionException(ex ? ex.what().c_str() : "RS closing"));
    resumeCallback_.reset();
  }

  auto connectionEvents = std::move(connectionEvents_);
  if (connectionEvents) {
    connectionEvents->onClosed(ex);
  }

  closeStreams(signal);
  closeFrameTransport(std::move(ex), signal);
}

void RSocketStateMachine::closeFrameTransport(
    folly::exception_wrapper ex,
    StreamCompletionSignal signal) {
  if (isDisconnectedOrClosed()) {
    DCHECK(!resumeCallback_);
    return;
  }

  // Stop scheduling keepalives since the socket is now disconnected
  if (keepaliveTimer_) {
    keepaliveTimer_->stop();
  }

  if (resumeCallback_) {
    resumeCallback_->onResumeError(
        ConnectionException(ex ? ex.what().c_str() : "connection closing"));
    resumeCallback_.reset();
  }

  // Echo the exception to the frameTransport only if the frameTransport started
  // closing with error.  Otherwise we sent some error frame over the wire and
  // we are closing the transport cleanly.
  if (signal == StreamCompletionSignal::CONNECTION_ERROR) {
    frameTransport_->closeWithError(std::move(ex));
  } else {
    frameTransport_->close();
  }

  frameTransport_ = nullptr;
}

void RSocketStateMachine::disconnectOrCloseWithError(Frame_ERROR&& errorFrame) {
  debugCheckCorrectExecutor();
  if (isResumable_) {
    disconnect(std::runtime_error(errorFrame.payload_.data->cloneAsValue()
                                      .moveToFbString()
                                      .toStdString()));
  } else {
    closeWithError(std::move(errorFrame));
  }
}

void RSocketStateMachine::closeWithError(Frame_ERROR&& error) {
  debugCheckCorrectExecutor();
  VLOG(3) << "closeWithError "
          << error.payload_.data->cloneAsValue().moveToFbString();

  StreamCompletionSignal signal;
  switch (error.errorCode_) {
    case ErrorCode::INVALID_SETUP:
      signal = StreamCompletionSignal::INVALID_SETUP;
      break;
    case ErrorCode::UNSUPPORTED_SETUP:
      signal = StreamCompletionSignal::UNSUPPORTED_SETUP;
      break;
    case ErrorCode::REJECTED_SETUP:
      signal = StreamCompletionSignal::REJECTED_SETUP;
      break;

    case ErrorCode::CONNECTION_ERROR:
    // StreamCompletionSignal::CONNECTION_ERROR is reserved for
    // frameTransport errors
    // ErrorCode::CONNECTION_ERROR is a normal Frame_ERROR error code which has
    // nothing to do with frameTransport
    case ErrorCode::APPLICATION_ERROR:
    case ErrorCode::REJECTED:
    case ErrorCode::RESERVED:
    case ErrorCode::CANCELED:
    case ErrorCode::INVALID:
    default:
      signal = StreamCompletionSignal::ERROR;
  }

  auto exception = std::runtime_error(
      error.payload_.data->cloneAsValue().moveToFbString().toStdString());

  if (frameSerializer_) {
    outputFrameOrEnqueue(std::move(error));
  }
  close(std::move(exception), signal);
}

void RSocketStateMachine::reconnect(
    yarpl::Reference<FrameTransport> newFrameTransport,
    std::unique_ptr<ClientResumeStatusCallback> resumeCallback) {
  debugCheckCorrectExecutor();
  CHECK(newFrameTransport);
  CHECK(resumeCallback);
  CHECK(!resumeCallback_);
  CHECK(isResumable_);
  CHECK(mode_ == ReactiveSocketMode::CLIENT);

  // TODO: output frame buffer should not be written to the new connection until
  // we receive resume ok
  resumeCallback_ = std::move(resumeCallback);
  connect(std::move(newFrameTransport), false, ProtocolVersion::Unknown);
}

void RSocketStateMachine::addStream(
    StreamId streamId,
    yarpl::Reference<StreamStateMachineBase> stateMachine) {
  debugCheckCorrectExecutor();
  auto result =
      streamState_.streams_.emplace(streamId, std::move(stateMachine));
  DCHECK(result.second);
}

void RSocketStateMachine::endStream(
    StreamId streamId,
    StreamCompletionSignal signal) {
  debugCheckCorrectExecutor();
  VLOG(6) << "endStream";
  // The signal must be idempotent.
  if (!endStreamInternal(streamId, signal)) {
    return;
  }
  DCHECK(
      signal == StreamCompletionSignal::CANCEL ||
      signal == StreamCompletionSignal::COMPLETE ||
      signal == StreamCompletionSignal::APPLICATION_ERROR ||
      signal == StreamCompletionSignal::ERROR);
}

bool RSocketStateMachine::endStreamInternal(
    StreamId streamId,
    StreamCompletionSignal signal) {
  VLOG(6) << "endStreamInternal";
  auto it = streamState_.streams_.find(streamId);
  if (it == streamState_.streams_.end()) {
    // Unsubscribe handshake initiated by the connection, we're done.
    return false;
  }

  resumeManager_->onStreamClosed(streamId);

  // Remove from the map before notifying the stateMachine.
  auto stateMachine = std::move(it->second);
  streamState_.streams_.erase(it);
  stateMachine->endStream(signal);
  return true;
}

void RSocketStateMachine::closeStreams(StreamCompletionSignal signal) {
  // Close all streams.
  while (!streamState_.streams_.empty()) {
    auto oldSize = streamState_.streams_.size();
    auto result =
        endStreamInternal(streamState_.streams_.begin()->first, signal);
    // TODO(stupaq): what kind of a user action could violate these
    // assertions?
    DCHECK(result);
    DCHECK_EQ(streamState_.streams_.size(), oldSize - 1);
  }
}

void RSocketStateMachine::processFrame(std::unique_ptr<folly::IOBuf> frame) {
  auto thisPtr = this->shared_from_this();
  executor_.add([ thisPtr, frame = std::move(frame) ]() mutable {
    thisPtr->processFrameImpl(std::move(frame));
  });
}

void RSocketStateMachine::processFrameImpl(
    std::unique_ptr<folly::IOBuf> frame) {
  if (isClosed()) {
    return;
  }

  if (!ensureOrAutodetectFrameSerializer(*frame)) {
    constexpr folly::StringPiece message{"Cannot detect protocol version"};
    closeWithError(Frame_ERROR::connectionError(message.str()));
    return;
  }

  auto frameType = frameSerializer_->peekFrameType(*frame);
  stats_->frameRead(frameType);

  auto streamIdPtr = frameSerializer_->peekStreamId(*frame);
  if (!streamIdPtr) {
    constexpr folly::StringPiece message{"Cannot decode stream ID"};
    closeWithError(Frame_ERROR::connectionError(message.str()));
    return;
  }

  auto streamId = *streamIdPtr;
  resumeManager_->trackReceivedFrame(*frame, frameType, streamId);
  if (streamId == 0) {
    handleConnectionFrame(frameType, std::move(frame));
    return;
  }

  // during the time when we are resuming we are can't receive any other
  // than connection level frames which drives the resumption
  // TODO(lehecka): this assertion should be handled more elegantly using
  // different state machine
  if (resumeCallback_) {
    constexpr folly::StringPiece message{
        "Received stream frame while resuming"};
    LOG(ERROR) << message;
    closeWithError(Frame_ERROR::connectionError(message.str()));
    return;
  }

  handleStreamFrame(streamId, frameType, std::move(frame));
}

void RSocketStateMachine::onTerminal(folly::exception_wrapper ex) {
  auto thisPtr = this->shared_from_this();
  executor_.add([ thisPtr, e = std::move(ex) ]() mutable {
    thisPtr->onTerminalImpl(std::move(e));
  });
}

void RSocketStateMachine::onTerminalImpl(folly::exception_wrapper ex) {
  if (isResumable_) {
    disconnect(std::move(ex));
  } else {
    auto termSignal = ex ? StreamCompletionSignal::CONNECTION_ERROR
                         : StreamCompletionSignal::CONNECTION_END;
    close(std::move(ex), termSignal);
  }
}

void RSocketStateMachine::handleConnectionFrame(
    FrameType frameType,
    std::unique_ptr<folly::IOBuf> payload) {
  switch (frameType) {
    case FrameType::KEEPALIVE: {
      Frame_KEEPALIVE frame;
      if (!deserializeFrameOrError(
              remoteResumeable_, frame, std::move(payload))) {
        return;
      }
      VLOG(3) << "In: " << frame;
      resumeManager_->resetUpToPosition(frame.position_);
      if (mode_ == ReactiveSocketMode::SERVER) {
        if (!!(frame.header_.flags_ & FrameFlags::KEEPALIVE_RESPOND)) {
          sendKeepalive(FrameFlags::EMPTY, std::move(frame.data_));
        } else {
          closeWithError(
              Frame_ERROR::connectionError("keepalive without flag"));
        }
      } else {
        if (!!(frame.header_.flags_ & FrameFlags::KEEPALIVE_RESPOND)) {
          closeWithError(Frame_ERROR::connectionError(
              "client received keepalive with respond flag"));
        } else if (keepaliveTimer_) {
          keepaliveTimer_->keepaliveReceived();
        }
      }
      return;
    }
    case FrameType::METADATA_PUSH: {
      Frame_METADATA_PUSH frame;
      if (deserializeFrameOrError(frame, std::move(payload))) {
        VLOG(3) << "In: " << frame;
        requestResponder_->handleMetadataPush(std::move(frame.metadata_));
      }
      return;
    }
    case FrameType::RESUME_OK: {
      Frame_RESUME_OK frame;
      if (!deserializeFrameOrError(frame, std::move(payload))) {
        return;
      }
      VLOG(3) << "In: " << frame;

      if (!resumeCallback_) {
        constexpr folly::StringPiece message{
            "Received RESUME_OK while not resuming"};
        closeWithError(Frame_ERROR::connectionError(message.str()));
        return;
      }

      if (!resumeManager_->isPositionAvailable(frame.position_)) {
        auto message = folly::sformat(
            "Client cannot resume, server position {} is not available",
            frame.position_);
        closeWithError(Frame_ERROR::connectionError(std::move(message)));
        return;
      }

      resumeCallback_->onResumeOk();
      resumeCallback_.reset();
      resumeFromPosition(frame.position_);
      return;
    }
    case FrameType::ERROR: {
      Frame_ERROR frame;
      if (!deserializeFrameOrError(frame, std::move(payload))) {
        return;
      }
      VLOG(3) << "In: " << frame;

      // TODO: handle INVALID_SETUP, UNSUPPORTED_SETUP, REJECTED_SETUP

      if ((frame.errorCode_ == ErrorCode::CONNECTION_ERROR ||
           frame.errorCode_ == ErrorCode::REJECTED_RESUME) &&
          resumeCallback_) {
        resumeCallback_->onResumeError(
            ResumptionException(frame.payload_.moveDataToString()));
        resumeCallback_.reset();
        // fall through
      }

      close(
          std::runtime_error(frame.payload_.moveDataToString()),
          StreamCompletionSignal::ERROR);
      return;
    }
    case FrameType::SETUP: // this should be processed in SetupResumeAcceptor
    case FrameType::RESUME: // this should be processed in SetupResumeAcceptor
    case FrameType::RESERVED:
    case FrameType::LEASE:
    case FrameType::REQUEST_RESPONSE:
    case FrameType::REQUEST_FNF:
    case FrameType::REQUEST_STREAM:
    case FrameType::REQUEST_CHANNEL:
    case FrameType::REQUEST_N:
    case FrameType::CANCEL:
    case FrameType::PAYLOAD:
    case FrameType::EXT:
    default: {
      std::stringstream message;
      message << "Unexpected " << frameType << " frame for stream 0";
      closeWithError(Frame_ERROR::connectionError(message.str()));
      return;
    }
  }
}

void RSocketStateMachine::handleStreamFrame(
    StreamId streamId,
    FrameType frameType,
    std::unique_ptr<folly::IOBuf> serializedFrame) {
  auto it = streamState_.streams_.find(streamId);
  if (it == streamState_.streams_.end()) {
    handleUnknownStream(streamId, frameType, std::move(serializedFrame));
    return;
  }

  // we are purposely making a copy of the reference here to avoid problems with
  // lifetime of the stateMachine when a terminating signal is delivered which
  // will cause the stateMachine to be destroyed while in one of its methods
  auto stateMachine = it->second;

  switch (frameType) {
    case FrameType::REQUEST_N: {
      Frame_REQUEST_N frameRequestN;
      if (!deserializeFrameOrError(frameRequestN, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << "In: " << frameRequestN;
      stateMachine->handleRequestN(frameRequestN.requestN_);
      break;
    }
    case FrameType::CANCEL: {
      VLOG(3) << "In: " << Frame_CANCEL();
      stateMachine->handleCancel();
      break;
    }
    case FrameType::PAYLOAD: {
      Frame_PAYLOAD framePayload;
      if (!deserializeFrameOrError(framePayload, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << "In: " << framePayload;
      stateMachine->handlePayload(
          std::move(framePayload.payload_),
          framePayload.header_.flagsComplete(),
          framePayload.header_.flagsNext());
      break;
    }
    case FrameType::ERROR: {
      Frame_ERROR frameError;
      if (!deserializeFrameOrError(frameError, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << "In: " << frameError;
      stateMachine->handleError(
          std::runtime_error(frameError.payload_.moveDataToString()));
      break;
    }
    case FrameType::REQUEST_CHANNEL:
    case FrameType::REQUEST_RESPONSE:
    case FrameType::RESERVED:
    case FrameType::SETUP:
    case FrameType::LEASE:
    case FrameType::KEEPALIVE:
    case FrameType::REQUEST_FNF:
    case FrameType::REQUEST_STREAM:
    case FrameType::METADATA_PUSH:
    case FrameType::RESUME:
    case FrameType::RESUME_OK:
    case FrameType::EXT: {
      std::stringstream message;
      message << "Unexpected " << frameType << " frame for stream " << streamId;
      closeWithError(Frame_ERROR::connectionError(message.str()));
      break;
    }
    default:
      // Ignore unknown frames for compatibility with future frame types.
      break;
  }
}

void RSocketStateMachine::handleUnknownStream(
    StreamId streamId,
    FrameType frameType,
    std::unique_ptr<folly::IOBuf> serializedFrame) {
  DCHECK(streamId != 0);
  // TODO: comparing string versions is odd because from version
  // 10.0 the lexicographic comparison doesn't work
  // we should change the version to struct
  if (frameSerializer_->protocolVersion() > ProtocolVersion{0, 0} &&
      !streamsFactory_.registerNewPeerStreamId(streamId)) {
    return;
  }

  switch (frameType) {
    case FrameType::REQUEST_CHANNEL: {
      Frame_REQUEST_CHANNEL frame;
      if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << "In: " << frame;
      auto stateMachine =
          streamsFactory_.createChannelResponder(frame.requestN_, streamId);
      auto requestSink = requestResponder_->handleRequestChannelCore(
          std::move(frame.payload_), streamId, stateMachine);
      stateMachine->subscribe(requestSink);
      break;
    }
    case FrameType::REQUEST_STREAM: {
      Frame_REQUEST_STREAM frame;
      if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << "In: " << frame;
      auto stateMachine =
          streamsFactory_.createStreamResponder(frame.requestN_, streamId);
      requestResponder_->handleRequestStreamCore(
          std::move(frame.payload_), streamId, stateMachine);
      break;
    }
    case FrameType::REQUEST_RESPONSE: {
      Frame_REQUEST_RESPONSE frame;
      if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << "In: " << frame;
      auto stateMachine =
          streamsFactory_.createRequestResponseResponder(streamId);
      requestResponder_->handleRequestResponseCore(
          std::move(frame.payload_), streamId, stateMachine);
      break;
    }
    case FrameType::REQUEST_FNF: {
      Frame_REQUEST_FNF frame;
      if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << "In: " << frame;
      // no stream tracking is necessary
      requestResponder_->handleFireAndForget(
          std::move(frame.payload_), streamId);
      break;
    }

    case FrameType::RESUME:
    case FrameType::SETUP:
    case FrameType::METADATA_PUSH:
    case FrameType::LEASE:
    case FrameType::KEEPALIVE:
    case FrameType::RESERVED:
    case FrameType::REQUEST_N:
    case FrameType::CANCEL:
    case FrameType::PAYLOAD:
    case FrameType::ERROR:
    case FrameType::RESUME_OK:
    case FrameType::EXT:
    default:
      std::stringstream message;
      message << "Unexpected frame " << frameType << " for stream " << streamId;
      DLOG(ERROR) << message.str();
      closeWithError(Frame_ERROR::connectionError(message.str()));
      break;
  }
}

void RSocketStateMachine::sendKeepalive(std::unique_ptr<folly::IOBuf> data) {
  sendKeepalive(FrameFlags::KEEPALIVE_RESPOND, std::move(data));
}

void RSocketStateMachine::sendKeepalive(
    FrameFlags flags,
    std::unique_ptr<folly::IOBuf> data) {
  debugCheckCorrectExecutor();
  Frame_KEEPALIVE pingFrame(
      flags, resumeManager_->impliedPosition(), std::move(data));
  VLOG(3) << "Out: " << pingFrame;
  outputFrameOrEnqueue(
      frameSerializer_->serializeOut(std::move(pingFrame), remoteResumeable_));
}

void RSocketStateMachine::tryClientResume(
    const ResumeIdentificationToken& token,
    yarpl::Reference<FrameTransport> frameTransport,
    std::unique_ptr<ClientResumeStatusCallback> resumeCallback) {
  Frame_RESUME resumeFrame(
      token,
      resumeManager_->impliedPosition(),
      resumeManager_->firstSentPosition(),
      frameSerializer_->protocolVersion());
  VLOG(3) << "Out: " << resumeFrame;
  frameTransport->outputFrameOrEnqueue(
      frameSerializer_->serializeOut(std::move(resumeFrame)));

  // if the client was still connected we will disconnected the old connection
  // with a clear error message
  disconnect(std::runtime_error("resuming client on a different connection"));
  setResumable(true);
  reconnect(std::move(frameTransport), std::move(resumeCallback));
}

bool RSocketStateMachine::isPositionAvailable(ResumePosition position) {
  debugCheckCorrectExecutor();
  return resumeManager_->isPositionAvailable(position);
}

bool RSocketStateMachine::resumeFromPositionOrClose(
    ResumePosition serverPosition,
    ResumePosition clientPosition) {
  debugCheckCorrectExecutor();
  DCHECK(!resumeCallback_);
  DCHECK(!isDisconnectedOrClosed());
  DCHECK(mode_ == ReactiveSocketMode::SERVER);

  bool clientPositionExist = (clientPosition == kUnspecifiedResumePosition) ||
      clientPosition <= resumeManager_->impliedPosition();

  if (clientPositionExist &&
      resumeManager_->isPositionAvailable(serverPosition)) {
    Frame_RESUME_OK resumeOkFrame(resumeManager_->impliedPosition());
    VLOG(3) << "Out: " << resumeOkFrame;
    frameTransport_->outputFrameOrEnqueue(
        frameSerializer_->serializeOut(std::move(resumeOkFrame)));
    resumeFromPosition(serverPosition);
    return true;
  } else {
    closeWithError(Frame_ERROR::connectionError(folly::to<std::string>(
        "Cannot resume server, client lastServerPosition=",
        serverPosition,
        " firstClientPosition=",
        clientPosition,
        " is not available. Last reset position is ",
        resumeManager_->firstSentPosition())));
    return false;
  }
}

void RSocketStateMachine::resumeFromPosition(ResumePosition position) {
  DCHECK(!resumeCallback_);
  DCHECK(!isDisconnectedOrClosed());
  DCHECK(resumeManager_->isPositionAvailable(position));

  if (connectionEvents_) {
    connectionEvents_->onStreamsResumed();
  }
  resumeManager_->sendFramesFromPosition(position, *frameTransport_);

  for (auto& frame : streamState_.moveOutputPendingFrames()) {
    outputFrameOrEnqueue(std::move(frame));
  }

  if (!isDisconnectedOrClosed() && keepaliveTimer_) {
    keepaliveTimer_->start(shared_from_this());
  }
}

void RSocketStateMachine::outputFrameOrEnqueue(
    std::unique_ptr<folly::IOBuf> frame) {
  debugCheckCorrectExecutor();
  // if we are resuming we cant send any frames until we receive RESUME_OK
  if (!isDisconnectedOrClosed() && !resumeCallback_) {
    outputFrame(std::move(frame));
  } else {
    streamState_.enqueueOutputPendingFrame(std::move(frame));
  }
}

void RSocketStateMachine::requestFireAndForget(Payload request) {
  Frame_REQUEST_FNF frame(
      streamsFactory().getNextStreamId(),
      FrameFlags::EMPTY,
      std::move(std::move(request)));
  outputFrameOrEnqueue(std::move(frame));
}

void RSocketStateMachine::metadataPush(std::unique_ptr<folly::IOBuf> metadata) {
  Frame_METADATA_PUSH metadataPushFrame(std::move(metadata));
  outputFrameOrEnqueue(std::move(metadataPushFrame));
}

void RSocketStateMachine::outputFrame(std::unique_ptr<folly::IOBuf> frame) {
  DCHECK(!isDisconnectedOrClosed());

  auto frameType = frameSerializer_->peekFrameType(*frame);
  stats_->frameWritten(frameType);

  if (isResumable_) {
    auto streamIdPtr = frameSerializer_->peekStreamId(*frame);
    resumeManager_->trackSentFrame(*frame, frameType, streamIdPtr);
  }
  frameTransport_->outputFrameOrEnqueue(std::move(frame));
}

uint32_t RSocketStateMachine::getKeepaliveTime() const {
  debugCheckCorrectExecutor();
  return keepaliveTimer_
      ? static_cast<uint32_t>(keepaliveTimer_->keepaliveTime().count())
      : Frame_SETUP::kMaxKeepaliveTime;
}

bool RSocketStateMachine::isDisconnectedOrClosed() const {
  return !frameTransport_;
}

bool RSocketStateMachine::isClosed() const {
  return isClosed_;
}

void RSocketStateMachine::debugCheckCorrectExecutor() const {
  DCHECK(
      !dynamic_cast<folly::EventBase*>(&executor_) ||
      dynamic_cast<folly::EventBase*>(&executor_)->isInEventBaseThread());
}

void RSocketStateMachine::setFrameSerializer(
    std::unique_ptr<FrameSerializer> frameSerializer) {
  CHECK(frameSerializer);
  // serializer is not interchangeable, it would screw up resumability
  // CHECK(!frameSerializer_);
  frameSerializer_ = std::move(frameSerializer);
}

void RSocketStateMachine::connectClientSendSetup(
    std::unique_ptr<DuplexConnection> connection,
    SetupParameters setupParams) {
  setFrameSerializer(
      setupParams.protocolVersion == ProtocolVersion::Unknown
          ? FrameSerializer::createCurrentVersion()
          : FrameSerializer::createFrameSerializer(
                setupParams.protocolVersion));

  setResumable(setupParams.resumable);

  auto frameTransport = yarpl::make_ref<FrameTransport>(std::move(connection));

  auto protocolVersion = frameSerializer_->protocolVersion();

  Frame_SETUP frame(
      setupParams.resumable ? FrameFlags::RESUME_ENABLE : FrameFlags::EMPTY,
      protocolVersion.major,
      protocolVersion.minor,
      getKeepaliveTime(),
      Frame_SETUP::kMaxLifetime,
      std::move(setupParams.token),
      std::move(setupParams.metadataMimeType),
      std::move(setupParams.dataMimeType),
      std::move(setupParams.payload));

  // TODO: when the server returns back that it doesn't support resumability, we
  // should retry without resumability

  VLOG(3) << "Out: " << frame;
  // making sure we send setup frame first
  frameTransport->outputFrameOrEnqueue(
      frameSerializer_->serializeOut(std::move(frame)));
  // then the rest of the cached frames will be sent
  connect(std::move(frameTransport), true, ProtocolVersion::Unknown);
}

bool RSocketStateMachine::isInEventBaseThread() {
  return dynamic_cast<folly::EventBase*>(&executor_)->isInEventBaseThread();
}

void RSocketStateMachine::writeNewStream(
    StreamId streamId,
    StreamType streamType,
    uint32_t initialRequestN,
    Payload payload,
    bool completed) {
  switch (streamType) {
    case StreamType::CHANNEL:
      outputFrameOrEnqueue(Frame_REQUEST_CHANNEL(
          streamId,
          completed ? FrameFlags::COMPLETE : FrameFlags::EMPTY,
          initialRequestN,
          std::move(payload)));
      break;

    case StreamType::STREAM:
      outputFrameOrEnqueue(Frame_REQUEST_STREAM(
          streamId, FrameFlags::EMPTY, initialRequestN, std::move(payload)));
      break;

    case StreamType::REQUEST_RESPONSE:
      outputFrameOrEnqueue(Frame_REQUEST_RESPONSE(
          streamId, FrameFlags::EMPTY, std::move(payload)));
      break;

    case StreamType::FNF:
      outputFrameOrEnqueue(
          Frame_REQUEST_FNF(streamId, FrameFlags::EMPTY, std::move(payload)));
      break;

    default:
      CHECK(false); // unknown type
  }
}

void RSocketStateMachine::writeRequestN(StreamId streamId, uint32_t n) {
  outputFrameOrEnqueue(Frame_REQUEST_N(streamId, n));
}

void RSocketStateMachine::writePayload(
    StreamId streamId,
    Payload payload,
    bool complete) {
  Frame_PAYLOAD frame(
      streamId,
      FrameFlags::NEXT | (complete ? FrameFlags::COMPLETE : FrameFlags::EMPTY),
      std::move(payload));
  outputFrameOrEnqueue(std::move(frame));
}

void RSocketStateMachine::writeCloseStream(
    StreamId streamId,
    StreamCompletionSignal signal,
    std::string message) {
  switch (signal) {
    case StreamCompletionSignal::COMPLETE:
      outputFrameOrEnqueue(Frame_PAYLOAD::complete(streamId));
      break;

    case StreamCompletionSignal::CANCEL:
      outputFrameOrEnqueue(Frame_CANCEL(streamId));
      break;

    case StreamCompletionSignal::ERROR:
      outputFrameOrEnqueue(Frame_ERROR::invalid(streamId, std::move(message)));
      break;

    case StreamCompletionSignal::APPLICATION_ERROR:
      outputFrameOrEnqueue(
          Frame_ERROR::applicationError(streamId, std::move(message)));
      break;

    case StreamCompletionSignal::INVALID_SETUP:
    case StreamCompletionSignal::UNSUPPORTED_SETUP:
    case StreamCompletionSignal::REJECTED_SETUP:

    case StreamCompletionSignal::CONNECTION_ERROR:
    case StreamCompletionSignal::CONNECTION_END:
    case StreamCompletionSignal::SOCKET_CLOSED:
    default:
      CHECK(false); // unexpected value
  }
}

void RSocketStateMachine::onStreamClosed(
    StreamId streamId,
    StreamCompletionSignal signal) {
  endStream(streamId, signal);
}

bool RSocketStateMachine::ensureOrAutodetectFrameSerializer(
    const folly::IOBuf& firstFrame) {
  if (frameSerializer_) {
    return true;
  }

  if (mode_ != ReactiveSocketMode::SERVER) {
    // this should never happen as clients are initized with FrameSerializer
    // instance
    DCHECK(false);
    return false;
  }

  auto serializer = FrameSerializer::createAutodetectedSerializer(firstFrame);
  if (!serializer) {
    LOG(ERROR) << "unable to detect protocol version";
    return false;
  }

  VLOG(2) << "detected protocol version" << serializer->protocolVersion();
  frameSerializer_ = std::move(serializer);
  return true;
}
} // namespace rsocket
