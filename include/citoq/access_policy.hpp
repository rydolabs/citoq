#pragma once

#include <citoq/interest.hpp>

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace citoq {

// Exact policy evaluation only. A verified authenticated principal must be
// supplied externally; this type does NOT authenticate a TLS peer.
enum class AccessOperation {
    Publish,
    Subscribe,
    Request,
    Handle,
};

struct AccessRule {
    std::string principal;
    AccessOperation operation{AccessOperation::Publish};
    InterestKey key{};

    bool operator==(const AccessRule&) const noexcept = default;
};

struct AccessRuleHash {
    std::size_t operator()(const AccessRule& rule) const noexcept {
        const auto a = std::hash<std::string>{}(rule.principal);
        const auto b = InterestKeyHash{}(rule.key);
        const auto c = static_cast<std::size_t>(rule.operation);
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2))
                 ^ (c * 0x517cc1b727220a95ULL);
    }
};

// Deny-by-default, bounded grants for a previously authenticated identity.
// Does not implement wildcard or grant persistence.
class AccessPolicy {
public:
    explicit AccessPolicy(std::size_t max_grants = 4096)
        : max_grants_(max_grants) {}

    bool grant(std::string principal,
               AccessOperation operation,
               InterestKey key) {
        if (principal.empty()) {
            throw std::invalid_argument("CitoQ ACL principal is empty");
        }
        AccessRule rule{std::move(principal), operation, key};
        if (grants_.contains(rule)) return false;
        if (grants_.size() >= max_grants_) {
            throw std::length_error("CitoQ ACL capacity exceeded");
        }
        return grants_.insert(std::move(rule)).second;
    }

    bool revoke(std::string_view principal,
                AccessOperation operation,
                InterestKey key) {
        return grants_.erase(AccessRule{
            std::string(principal), operation, key}) != 0;
    }

    bool allows(std::string_view principal,
                AccessOperation operation,
                InterestKey key) const {
        if (principal.empty()) return false;
        return grants_.contains(AccessRule{
            std::string(principal), operation, key});
    }

    void clear() noexcept { grants_.clear(); }
    std::size_t size() const noexcept { return grants_.size(); }
    std::size_t capacity() const noexcept { return max_grants_; }

private:
    const std::size_t max_grants_;
    std::unordered_set<AccessRule, AccessRuleHash> grants_;
};

} // namespace citoq
