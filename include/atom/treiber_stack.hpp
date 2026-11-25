#pragma once

#include <atomic>
#include <optional>
#include <utility>

#include "atom/config.hpp"

namespace atom {

// Reclamation policy that never frees a popped node (milestone M6). Correct, because a node
// that is never freed can never be read after free and never come back at the same address
// (no ABA), but it leaks every popped node. It exists to show that reclamation is the whole
// difficulty: the stack algorithm itself is the same with every policy.
struct NoReclaim {
    class Guard {
    public:
        template <class N>
        N* protect(const std::atomic<N*>& src) noexcept {
            // acquire: pairs with the release that published the node, so its fields are visible.
            return src.load(std::memory_order_acquire);
        }
        void reset() noexcept {}
    };
    template <class N>
    static void retire(N*) noexcept {}  // leak on purpose
};

// Lock-free LIFO stack (R. Kent Treiber, 1986). One CAS on head_ per push and per pop.
//
// The hard part is pop: it reads h->next from a node that another thread may pop and free at
// the same moment (use-after-free), and the CAS cannot tell whether head_ went A -> B -> A
// in between (ABA). The Reclaim policy answers both: a node is never freed while a thread may
// still read it, so it can never be reused while that thread's CAS still expects it.
//
//   Reclaim = NoReclaim       leaks popped nodes (M6)
//   Reclaim = HazardPointers  reclaim/hazard_pointer.hpp (M7)
//   Reclaim = EpochReclaim    reclaim/epoch.hpp (M8)
template <class T, class Reclaim = NoReclaim>
class TreiberStack {
    struct Node {
        T     value;
        Node* next = nullptr;  // written only before the node is published, then read-only
    };

public:
    using value_type = T;

    TreiberStack() = default;

    // Not thread-safe: every other thread must be finished with the stack.
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
        n->next = head_.load(std::memory_order_relaxed);  // a guess; the CAS checks it
        // success release: publishes n's value and next to any thread that loads n from head_.
        // failure relaxed: the CAS stored the current head in n->next; just try again.
        while (!head_.compare_exchange_weak(n->next, n, std::memory_order_release,
                                            std::memory_order_relaxed)) {
        }
    }

    std::optional<T> pop() {
        typename Reclaim::Guard guard;
        for (;;) {
            Node* h = guard.protect(head_);
            if (h == nullptr) return std::nullopt;
            Node* next = h->next;  // safe: the guard keeps h from being freed
            // success acq_rel: acquire as usual for reading h's fields; release so that the
            // chain of CASes on head_ keeps carrying each push's release forward to the next
            // thread that loads head_ (it may load `next`, published by an older push).
            // failure relaxed: protect() reloads head_ with the ordering it needs.
            if (head_.compare_exchange_weak(h, next, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                std::optional<T> out(std::move(h->value));  // read before retire may free h
                guard.reset();
                Reclaim::retire(h);  // h is unlinked and only we hold it now
                return out;
            }
        }
    }

    bool empty() const noexcept { return head_.load(std::memory_order_acquire) == nullptr; }

private:
    alignas(kCacheLine) std::atomic<Node*> head_{nullptr};
};

}  // namespace atom
