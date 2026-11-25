#pragma once

#include <atomic>
#include <optional>
#include <utility>

#include "atom/config.hpp"

namespace atom {

// Never frees popped nodes: correct, but leaks.
struct NoReclaim {
    class Guard {
    public:
        template <class N>
        N* protect(const std::atomic<N*>& src) noexcept {
            return src.load(std::memory_order_acquire);
        }
        void reset() noexcept {}
    };
    template <class N>
    static void retire(N*) noexcept {}
};

// Reclaim: NoReclaim, HazardPointers or EpochReclaim.
template <class T, class Reclaim = NoReclaim>
class TreiberStack {
    struct Node {
        T     value;
        Node* next = nullptr;
    };

public:
    using value_type = T;

    TreiberStack() = default;

    ~TreiberStack() {
        Node* n = head_.load(std::memory_order_relaxed);
        while (n != nullptr) {
            Node* next = n->next;
            delete n;
            n = next;
        }
    }

    TreiberStack(const TreiberStack&)            = delete;
    TreiberStack& operator=(const TreiberStack&) = delete;

    template <class U>
    void push(U&& value) {
        Node* n = new Node{T(std::forward<U>(value)), nullptr};
        n->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(n->next, n, std::memory_order_release,
                                            std::memory_order_relaxed)) {
        }
    }

    std::optional<T> pop() {
        typename Reclaim::Guard guard;
        for (;;) {
            Node* h = guard.protect(head_);
            if (h == nullptr) return std::nullopt;
            Node* next = h->next;
            // acq_rel: release keeps older pushes visible to whoever loads `next` from head_.
            if (head_.compare_exchange_weak(h, next, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                std::optional<T> out(std::move(h->value));  // before retire may free h
                guard.reset();
                Reclaim::retire(h);
                return out;
            }
        }
    }

    bool empty() const noexcept { return head_.load(std::memory_order_acquire) == nullptr; }

private:
    alignas(kCacheLine) std::atomic<Node*> head_{nullptr};
};

}  // namespace atom
