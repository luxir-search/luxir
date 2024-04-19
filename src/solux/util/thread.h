#pragma once
#include "oneapi/tbb/flow_graph.h"

namespace solux {



// A simple (but heavyweight) class like a latch so that one can block waiting on an async action to complete.
// When this class waits, it enters the TBB work stealing loop.
class Blocker {
  tbb::flow::graph g;

  // If we don't need to pass data, it might be simpler (or maybe faster) to
  // just use graph.reserve_wait() and graph.release_wait() directly.

public:
  Blocker() : g() {
    g.reserve_wait();
  }

  // enter TBB work-stealing loop until someone else calls blocker.notify()
  void wait() {
    g.wait_for_all();  // this "blocks" (but enters TBB work stealing loop)
  }

  // call this to signal that the async work is done and that the wait() call can return.
  void notify() {
    g.release_wait(); // notifies flow graph that async work has been completed.
  }
};



// A simple (but heavyweight) class like a latch so that one can block waiting on an async action to complete.
// When this class waits, it enters the TBB work stealing loop.
class BlockerAsyncNode {
  using node_t = tbb::flow::async_node<void*, void*>;
  using gateway_t = node_t::gateway_type;
  tbb::flow::graph g;
  std::function<void()> action;
  node_t asyncNode;

  // If we don't need to pass data, it might be simpler (or maybe faster) to
  // just use graph.reserve_wait() and graph.release_wait() directly.

public:
  BlockerAsyncNode(std::function<void()> action) :
          action(std::move(action)),
          asyncNode(g, tbb::flow::unlimited, [this](void* input, gateway_t& gateway) {
            gateway.reserve_wait(); // notifies flow graph that async work has been submitted.  Do this first to avoid race conditions.
            this->action(); // do our action after so there is no possibility of notify() being called first!
          } )
  {
  }

  // call this to kick off the async work and then wait for someone to call notify()
  void wait() {
    asyncNode.try_put(nullptr);
    g.wait_for_all();  // this "blocks" (but enters TBB work stealing loop)
  }

  // call this to signal that the async work is done and that the wait() call can return.
  void notify() {
    asyncNode.gateway().release_wait(); // notifies flow graph that async work has been completed.
  }
};


}