#include "telemetry.hpp"

#include <cassert>
#include <cstdint>

int main() {
    acme::Telemetry value{};
    value.pose.x = 1.0;
    value.pose.y = 2.0;
    value.mode = acme::Mode::Active;
    value.coefficients = {1.0f, 2.0f, 3.0f, 4.0f};
    value.tags = {"alpha", "beta"};
    value.timestamp = std::uint64_t{99};

    const auto type = cito::type_of<acme::Telemetry>();
    assert(type.name() == "acme.Telemetry");
    assert(type.find("pose")->type.kind == cito::TypeKind::Struct);
    assert(type.find("mode")->type.kind == cito::TypeKind::Enum);
    assert(type.find("coefficients")->type.kind == cito::TypeKind::Array);
    assert(type.find("tags")->type.kind == cito::TypeKind::Sequence);

    const auto dynamic = cito::to_dynamic(value);
    const auto nested =
        std::get<cito::DynamicStruct>(dynamic.value("pose"));
    assert(nested->get<double>("x") == 1.0);

    const auto tags =
        std::get<cito::DynamicListPtr>(dynamic.value("tags"));
    assert(tags->values.size() == 2);

    static_assert(cito::StaticCodec<acme::Pose>::direct);
    static_assert(cito::StaticCodec<acme::Telemetry>::direct);

    const auto bytes = cito::encode(value);
    const auto reference_bytes =
        cito::wire::encode(cito::to_dynamic(value));
    assert(bytes == reference_bytes);

    const auto roundtrip = cito::decode<acme::Telemetry>(bytes);

    assert(roundtrip.pose.x == value.pose.x);
    assert(roundtrip.pose.y == value.pose.y);
    assert(roundtrip.mode == value.mode);
    assert(roundtrip.coefficients == value.coefficients);
    assert(roundtrip.tags == value.tags);
    assert(roundtrip.timestamp == value.timestamp);

    return 0;
}
