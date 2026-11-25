#pragma once

#include <atomic>
#include <utility>

#include "atom/config.hpp"
#include "atom/queue_concept.hpp"

namespace atom {

enum class PopResult {
    Item,   // an item was taken
    Empty,  // nothing has been pushed that is not already taken
    Retry,  // a producer is half-way through a push; an item is about to appear
};

// Many producers, one consumer: Dmitry Vyukov's linked MPSC queue. Unbounded; one
// `exchange` and one store per push, no retry loop.
//
// Producers append at head_; the consumer reads from tail_. The node tail_ points at has
// already been consumed (at first it is a dummy "stub"), so the list is never empty of nodes
// and push never has to special-case an empty queue. Only the consumer ever frees nodes, and
// it frees only nodes no producer can still reach, so no reclamation scheme is needed.
//
// Progress: push is wait-free. Pop is not lock-free in the strict sense: a producer paused
// between its exchange and its link leaves the list broken at that point, and the consumer
// sees nothing past it until that producer runs again. No item is lost, it is delayed.
// try_pop_ex reports that case as PopResult::Retry, so a caller can tell it from Empty.
template <QueueItem T>
class MpscQueue {
    struct Node {
        std::atomic<Node*> next{nullptr};
        T                  value{};

        Node() = default;
        template <class U>
        explicit Node(U&& v) : value(std::forward<U>(v)) {}
    };

public:
    using value_type = T;

    MpscQueue() {
        Node* stub = new Node;
        head_.store(stub, std::memory_order_relaxed);  // not shared yet
        tail_ = stub;
    }

    // Not thread-safe: every producer and the consumer must be finished.
    ~MpscQueue() {
        Node* n = tail_;
        while (n != nullptr) {
            Node* next = n->next.load(std::memory_order_relaxed);  // no other thread is left
            delete n;
            n = next;
        }
    }

    MpscQueue(const MpscQueue&)            = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // Any thread. Never fails (the queue is unbounded); returns bool for the shared interface.
    template <class U>
    bool try_push(U&& value) {
        push(std::forward<U>(value));
        return true;
    }

    template <class U>
    void push(U&& value) {
        Node* n = new Node(std::forward<U>(value));
        // Step 1, claim the end. Every concurrent producer gets a different `prev` back, so no
        // two ever link onto the same node.
        // acq_rel: release so the producer that gets `n` back as its prev sees n fully built
        // (it writes n->next); acquire for the same reason in the other direction on `prev`.
        Node* prev = head_.exchange(n, std::memory_order_acq_rel);
        // Step 2, link. release: publishes n's value to the consumer's acquire-load of next.
        prev->next.store(n, std::memory_order_release);
    }

    // Consumer only.
    PopResult try_pop_ex(T& out) {
        Node* tail = tail_;
        // acquire: pairs with step 2 of push, so next->value is fully visible.
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            // head_ has moved past tail_ if some producer finished step 1 but not step 2.
            // relaxed: this is only a hint; nothing is read through the pointer.
            return head_.load(std::memory_order_relaxed) == tail ? PopResult::Empty
                                                                 : PopResult::Retry;
        }
        out   = std::move(next->value);
        tail_ = next;  // `next` becomes the new consumed dummy
        // Safe: the only producer that ever touched `tail` linked it in step 2 above, which
        // we have already seen, and no producer can reach it from head_ any more.
        delete tail;
        return PopResult::Item;
    }

    bool try_pop(T& out) { return try_pop_ex(out) == PopResult::Item; }

private:
    // Producers fight over this line; keep the consumer's pointer off it.
    alignas(kCacheLine) std::atomic<Node*> head_{nullptr};
    alignas(kCacheLine) Node* tail_ = nullptr;  // consumer only
};

}  // namespace atom
