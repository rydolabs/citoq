#include <cito/dynamic.hpp>

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>

int main() {
    const auto type = cito::TypeBuilder("acme.navigation.Position")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .member<std::string>(3, "frame").bound(16)
        .member<std::uint64_t>(4, "timestamp").optional()
        .build();

    assert(type.name() == "acme.navigation.Position");
    assert(type.type_id() == 3445279360899966310ull);
    assert(type.schema_hash() == 17480316621561973532ull);
    assert(type.fields().size() == 4);
    assert(type.find("timestamp")->optional);
    assert(type.find("frame")->type.bound == 16);

    const auto same = cito::TypeBuilder("acme.navigation.Position")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .member<std::string>(3, "frame").bound(16)
        .member<std::uint64_t>(4, "timestamp").optional()
        .build();

    assert(type.type_id() == same.type_id());
    assert(type.schema_hash() == same.schema_hash());

    const auto evolved = cito::TypeBuilder("acme.navigation.Position")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .member<std::string>(3, "frame").bound(16)
        .member<std::uint64_t>(4, "timestamp").optional()
        .member<std::int32_t>(5, "quality").optional()
        .build();

    assert(type.type_id() == evolved.type_id());
    assert(type.schema_hash() != evolved.schema_hash());

    cito::DynamicData data(type);
    data["x"] = 1.0;
    data.set("y", 2.0);
    data["frame"] = "map";

    assert(data.get<double>("x") == 1.0);
    assert(data.get<double>("y") == 2.0);
    assert(data.get<std::string>("frame") == "map");
    assert(!data.has("timestamp"));

    bool bound_failed = false;
    try {
        data["frame"] = "this-string-is-longer-than-sixteen";
    } catch (const std::length_error&) {
        bound_failed = true;
    }
    assert(bound_failed);

    bool duplicate_failed = false;
    try {
        (void)cito::TypeBuilder("Bad")
            .member<double>(1, "x")
            .member<double>(1, "y")
            .build();
    } catch (const std::invalid_argument&) {
        duplicate_failed = true;
    }
    assert(duplicate_failed);

    return 0;
}
