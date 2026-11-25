#pragma once

#include <atomic>
#include <utility>

#include "atom/config.hpp"
#include "atom/queue_concept.hpp"

namespace atom {

enum class PopResult { Item, Empty, Retry };  // Retry: a push is half done

// Vyukov's MPSC queue. Pop can be held up by a producer paused mid-push.
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
        head_.store(stub, std::memory_order_relaxed);
        tail_ = stub;
    }

    ~MpscQueue() {
        Node* n = tail_;
        while (n != nullptr) {
            Node* next = n->next.load(std::memory_order_relaxed);
            delete n;
            n = next;
        }
    }

    MpscQueue(const MpscQueue&)            = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    template <class U>
    bool try_push(U&& value) {
        push(std::forward<U>(value));
        return true;
    }

    template <class U>
    void push(U&& value) {
        Node* n = new Node(std::forward<U>(value));
        // acq_rel: the next producer writes n->next, so n must be visible to it.
        Node* prev = head_.exchange(n, std::memory_order_acq_rel);
        prev->next.store(n, std::memory_order_release);
    }

    PopResult try_pop_ex(T& out) {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            return head_.load(std::memory_order_relaxed) == tail ? PopResult::Empty
                                                                 : PopResult::Retry;
        }
        out   = std::move(next->value);
        tail_ = next;
        delete tail;
        return PopResult::Item;
    }

    bool try_pop(T& out) { return try_pop_ex(out) == PopResult::Item; }

private:
    alignas(kCacheLine) std::atomic<Node*> head_{nullptr};  // producers
    alignas(kCacheLine) Node* tail_ = nullptr;              // consumer
};

}  // namespace atom
