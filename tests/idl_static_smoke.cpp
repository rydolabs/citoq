#include "position.hpp"
#include <cassert>
#include <cstdint>
#include <string>

int main() {
    acme::navigation::Position p{};
    p.x = 1.25;
    p.y = -2.5;
    p.frame = "map";
    p.timestamp = std::uint64_t{123456};
    p.frame_kind = acme::navigation::FrameKind::Map;

    const auto generated_type = cito::type_of<acme::navigation::Position>();
    assert(generated_type.name() == "acme.navigation.Position");
    assert(generated_type.type_id() == 3445279360899966310ull);
    assert(generated_type.schema_hash() == 5316556461892417623ull);
    assert(generated_type.find("x")->id == 1);
    assert(generated_type.find("timestamp")->optional);
    assert(generated_type.find("frame_kind")->type.kind == cito::TypeKind::Enum);

    const auto manual_enum = cito::EnumBuilder("acme.navigation.FrameKind")
        .value(0, "Map")
        .value(1, "Odom")
        .build();
    const auto manual_type = cito::TypeBuilder("acme.navigation.Position")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .member(3, "frame", cito::types::string(16))
        .member<std::uint64_t>(4, "timestamp").optional()
        .member(5, "frame_kind", cito::types::enumeration(manual_enum)).optional()
        .build();
    assert(generated_type.type_id() == manual_type.type_id());
    assert(generated_type.schema_hash() == manual_type.schema_hash());

    static_assert(cito::StaticCodec<acme::navigation::Position>::direct);

    const auto bytes = cito::encode(p);
    const auto reference_bytes =
        cito::wire::encode(cito::to_dynamic(p));
    assert(bytes == reference_bytes);

    const auto roundtrip = cito::decode<acme::navigation::Position>(bytes);
    assert(roundtrip.x == p.x);
    assert(roundtrip.y == p.y);
    assert(roundtrip.frame == p.frame);
    assert(roundtrip.timestamp == p.timestamp);
    assert(roundtrip.frame_kind == p.frame_kind);

    const auto old_type = cito::TypeBuilder("acme.navigation.Position")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .member<std::string>(3, "frame").bound(16)
        .build();
    const auto old_view = cito::wire::decode(old_type, bytes);
    assert(old_view.get<double>("x") == p.x);
    assert(old_view.get<std::string>("frame") == "map");

    cito::DynamicData dynamic(generated_type);
    dynamic.set("x", 9.0);
    dynamic.set("y", 8.0);
    dynamic.set("frame", "odom");
    dynamic.set("frame_kind", std::int32_t{1});
    const auto from_dynamic = cito::from_dynamic<acme::navigation::Position>(dynamic);
    assert(from_dynamic.x == 9.0);
    assert(from_dynamic.frame_kind == acme::navigation::FrameKind::Odom);
    assert(!from_dynamic.timestamp.has_value());

    return 0;
}
