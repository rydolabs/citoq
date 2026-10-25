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

    const auto type = citoq::type_of<acme::Telemetry>();
    assert(type.name() == "acme.Telemetry");
    assert(type.find("pose")->type.kind == citoq::TypeKind::Struct);
    assert(type.find("mode")->type.kind == citoq::TypeKind::Enum);
    assert(type.find("coefficients")->type.kind == citoq::TypeKind::Array);
    assert(type.find("tags")->type.kind == citoq::TypeKind::Sequence);

    const auto dynamic = citoq::to_dynamic(value);
    const auto nested =
        std::get<citoq::DynamicStruct>(dynamic.value("pose"));
    assert(nested->get<double>("x") == 1.0);

    const auto tags =
        std::get<citoq::DynamicListPtr>(dynamic.value("tags"));
    assert(tags->values.size() == 2);

    static_assert(citoq::StaticCodec<acme::Pose>::direct);
    static_assert(citoq::StaticCodec<acme::Telemetry>::direct);

    const auto bytes = citoq::encode(value);
    const auto reference_bytes =
        citoq::wire::encode(citoq::to_dynamic(value));
    assert(bytes == reference_bytes);

    const auto roundtrip = citoq::decode<acme::Telemetry>(bytes);

    assert(roundtrip.pose.x == value.pose.x);
    assert(roundtrip.pose.y == value.pose.y);
    assert(roundtrip.mode == value.mode);
    assert(roundtrip.coefficients == value.coefficients);
    assert(roundtrip.tags == value.tags);
    assert(roundtrip.timestamp == value.timestamp);

    return 0;
}
