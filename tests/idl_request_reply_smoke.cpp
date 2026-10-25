#include "request_reply.hpp"

#include <citoq/static.hpp>

#include <cassert>
#include <type_traits>

int main() {
    static_assert(
        std::is_same_v<
            citoq::ReplyTypeT<
                acme::GetStatus>,
            acme::Status>);

    acme::GetStatus request{};
    request.drone_id = "drone_17";

    const auto bytes =
        citoq::encode(request);
    const auto decoded =
        citoq::decode<
            acme::GetStatus>(
                bytes);

    assert(
        decoded.drone_id ==
        "drone_17");

    return 0;
}
