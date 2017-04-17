#pragma once

#include "reactivestreams/ReactiveStreams.h"
#include "Refcounted.h"

namespace yarpl {

class Subscription : public reactivestreams_yarpl::Subscription,
    public virtual Refcounted {
public:
  virtual void request(int64_t n) = 0;
  virtual void cancel() = 0;

protected:
  Subscription() : reference_(this) {}

  // Drop the reference we're holding on the subscription (handle).
  void release() {
    reference_.reset();
  }

private:
  // We expect to be heap-allocated; until this subscription finishes
  // (is canceled; completes; error's out), hold a reference so we are
  // not deallocated (by the subscriber).
  Reference<Refcounted> reference_;
};

}  // yarpl