// Copyright 2004-present Facebook. All Rights Reserved.
#include <folly/String.h>
#include <folly/io/async/EventBaseManager.h>
#include <iostream>

#include <folly/init/Init.h>
#include <folly/portability/GFlags.h>
#include <glog/logging.h>

#include "rsocket/ConnectionAcceptor.h"
#include "rsocket/DuplexConnection.h"
#include "rsocket/RSocketServer.h"

struct FuzzerConnectionAcceptor : public rsocket::ConnectionAcceptor {
  void start(rsocket::OnDuplexConnectionAccept func) override {
    VLOG(1) << "FuzzerConnectionAcceptor::start()" << std::endl;
    func_ = func;
  }

  void stop() override {
    VLOG(1) << "FuzzerConnectionAcceptor::stop()" << std::endl;
  }

  folly::Optional<uint16_t> listeningPort() const override {
    return {0};
  }

  rsocket::OnDuplexConnectionAccept func_;
};

struct FuzzerDuplexConnection : public rsocket::DuplexConnection {
  using Subscriber = rsocket::DuplexConnection::Subscriber;

  struct SinkSubscriber : public Subscriber {
    std::vector<std::unique_ptr<folly::IOBuf>> sunk_buffers_;

    void onNext(std::unique_ptr<folly::IOBuf> buf) {
      VLOG(1) << "SinkSubscriber::onNext(\""
              << folly::humanify(buf->cloneAsValue().moveToFbString()) << "\")"
              << std::endl;
      sunk_buffers_.push_back(std::move(buf));
    }
  };

  FuzzerDuplexConnection() : output_sub_(yarpl::make_ref<SinkSubscriber>()) {}

  virtual void setInput(yarpl::Reference<Subscriber> sub) {
    VLOG(1) << "FuzzerDuplexConnection::setInput()" << std::endl;
    input_sub_ = sub;
  }
  virtual yarpl::Reference<Subscriber> getOutput() {
    VLOG(1) << "FuzzerDuplexConnection::getOutput()" << std::endl;
    return output_sub_;
  }

  yarpl::Reference<Subscriber> input_sub_;
  yarpl::Reference<Subscriber> output_sub_;
};

struct FuzzerSubscription : public yarpl::flowable::Subscription {
  void request(int64_t n) noexcept override {
    VLOG(1) << "FuzzerSubscription::request(" << n << ")";
  }
  void cancel() noexcept override {
    VLOG(1) << "FuzzerSubscription::cancel()";
  }
};

struct FuzzerResponder : public rsocket::RSocketResponder {};

std::string get_stdin() {
  std::cin >> std::noskipws;
  std::istream_iterator<char> it(std::cin);
  std::istream_iterator<char> end;
  std::string input(it, end);
  return input;
}

int main(int argc, char* argv[]) {
  folly::init(&argc, &argv);
  FLAGS_logtostderr = 1;

  folly::EventBase evb;
  folly::EventBaseManager::get()->setEventBase(&evb, false);

  auto feed_conn = std::make_unique<FuzzerDuplexConnection>();
  auto acceptor = std::make_unique<FuzzerConnectionAcceptor>();

  // grab references while we still own the duplex connection
  auto& input_sub = feed_conn->input_sub_;
  auto& output_sub = feed_conn->output_sub_;
  auto& acceptor_func_ptr = acceptor->func_;

  rsocket::RSocketServer server(std::move(acceptor));

  auto responder = std::make_shared<FuzzerResponder>();
  server.start(
      [responder](const rsocket::SetupParameters&) { return responder; });

  CHECK(acceptor_func_ptr);
  acceptor_func_ptr(std::move(feed_conn), evb);
  evb.loopOnce();

  CHECK(input_sub);
  CHECK(output_sub);
  auto input_subscription = yarpl::make_ref<FuzzerSubscription>();
  input_sub->onSubscribe(input_subscription);

#ifdef __AFL_HAVE_MANUAL_CONTROL
  __AFL_INIT();
#endif

  std::string fuzz_input = get_stdin();
  std::unique_ptr<folly::IOBuf> buf =
      folly::IOBuf::wrapBuffer(fuzz_input.c_str(), fuzz_input.size());

  VLOG(1) << "fuzz input: " << std::endl;
  VLOG(1) << folly::humanify(buf->cloneAsValue().moveToFbString()) << std::endl;

  input_sub->onNext(std::move(buf));
  evb.loopOnce();

  return 0;
}