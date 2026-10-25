#include "request_reply.hpp"

#include <cito/static.hpp>

#include <cassert>
#include <type_traits>

int main() {
    static_assert(
        std::is_same_v<
            cito::ReplyTypeT<
                acme::GetStatus>,
            acme::Status>);

    acme::GetStatus request{};
    request.drone_id = "drone_17";

    const auto bytes =
        cito::encode(request);
    const auto decoded =
        cito::decode<
            acme::GetStatus>(
                bytes);

    assert(
        decoded.drone_id ==
        "drone_17");

    return 0;
}
