// ============================================================================
// Multiplexer 在锁内移动 completion，在锁外执行用户等待逻辑。
// ============================================================================

#include "tudou/rpc/binary/Multiplexer.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace tudou {
namespace rpc {
namespace binary {

Multiplexer::Multiplexer()
    : mutex_(),
      pendingCalls_(),
      nextSequenceId_(1),
      open_(true) {
}

Multiplexer::~Multiplexer() = default;

uint64_t Multiplexer::register_call(Completion completion) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_.load()) {
        throw std::runtime_error("Multiplexer: Channel is closed");
    }

    const uint64_t sequenceId = nextSequenceId_++;
    pendingCalls_.emplace(sequenceId, std::move(completion));
    return sequenceId;
}

void Multiplexer::cancel_call(uint64_t sequenceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingCalls_.erase(sequenceId);
}

bool Multiplexer::complete_call(uint64_t sequenceId, const std::string& responseBody) {
    Completion completion;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pendingCalls_.find(sequenceId);
        if (it == pendingCalls_.end()) {
            return false;
        }
        completion = std::move(it->second);
        pendingCalls_.erase(it);
    }

    completion(responseBody, nullptr);
    return true;
}

void Multiplexer::close(std::exception_ptr error) {
    std::vector<Completion> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_.exchange(false)) {
            return;
        }

        pending.reserve(pendingCalls_.size());
        for (auto& entry : pendingCalls_) {
            pending.push_back(std::move(entry.second));
        }
        pendingCalls_.clear();
    }

    for (auto& completion : pending) {
        completion("", error);
    }
}

bool Multiplexer::is_open() const {
    return open_.load();
}

} // namespace binary
} // namespace rpc
} // namespace tudou
