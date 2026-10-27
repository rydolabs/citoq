#include <citoq/access_policy.hpp>
#include <citoq/identity.hpp>

#include <cassert>
#include <stdexcept>

int main() {
    using namespace citoq;
    const InterestKey pose{stable_name_id("acme"),
                           stable_name_id("drone_17"),
                           stable_name_id("acme.Position")};
    const InterestKey another{pose.scope,
                              stable_name_id("drone_18"),
                              pose.type};
    AccessPolicy policy(2);

    // Principal, Scope/Resource/Type, and operation must ALL match.
    assert(!policy.allows("alice", AccessOperation::Publish, pose));
    assert(!policy.allows("", AccessOperation::Publish, pose));
    assert(policy.grant("alice", AccessOperation::Publish, pose));
    assert(!policy.grant("alice", AccessOperation::Publish, pose));
    assert(policy.size() == 1);
    assert(policy.allows("alice", AccessOperation::Publish, pose));
    assert(!policy.allows("alice", AccessOperation::Subscribe, pose));
    assert(!policy.allows("alice", AccessOperation::Publish, another));
    assert(!policy.allows("bob", AccessOperation::Publish, pose));

    assert(policy.grant("alice", AccessOperation::Request, pose));
    assert(policy.size() == policy.capacity());
    bool capped = false;
    try {
        policy.grant("bob", AccessOperation::Handle, pose);
    } catch (const std::length_error&) {
        capped = true;
    }
    assert(capped);
    bool empty_rejected = false;
    try {
        policy.grant("", AccessOperation::Request, pose);
    } catch (const std::invalid_argument&) {
        empty_rejected = true;
    }
    assert(empty_rejected);
    assert(policy.revoke("alice", AccessOperation::Publish, pose));
    assert(!policy.allows("alice", AccessOperation::Publish, pose));
    assert(!policy.revoke("alice", AccessOperation::Publish, pose));
    assert(policy.grant("bob", AccessOperation::Handle, pose));
    assert(policy.allows("bob", AccessOperation::Handle, pose));
    policy.clear();
    assert(policy.size() == 0);
    assert(!policy.allows("bob", AccessOperation::Handle, pose));

    AccessPolicy deny_all(0);
    assert(!deny_all.allows("alice", AccessOperation::Publish, pose));
    return 0;
}
