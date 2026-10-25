#include <citoq/wire.hpp>

#include <cassert>
#include <cstdint>
#include <string>

static citoq::Type old_position() {
    return citoq::TypeBuilder("acme.navigation.Position")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .member<std::string>(3, "frame").bound(16)
        .build();
}

static citoq::Type new_position() {
    return citoq::TypeBuilder("acme.navigation.Position")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .member<std::string>(3, "frame").bound(16)
        .member<std::uint64_t>(4, "timestamp").optional()
        .build();
}

int main() {
    {
        auto type = old_position();
        citoq::DynamicData src(type);
        src["x"] = 1.25;
        src["y"] = -2.5;
        src["frame"] = "map";
        const auto bytes = citoq::wire::encode(src);
        const auto header = citoq::wire::inspect_header(bytes);
        assert(header.type_id == type.type_id());
        assert(header.schema_hash == type.schema_hash());
        assert(header.field_count == 3);
        auto dst = citoq::wire::decode(type, bytes);
        assert(dst.get<double>("x") == 1.25);
        assert(dst.get<double>("y") == -2.5);
        assert(dst.get<std::string>("frame") == "map");
    }

    {
        auto newer = new_position();
        citoq::DynamicData src(newer);
        src["x"] = 10.0; src["y"] = 20.0; src["frame"] = "odom";
        src["timestamp"] = std::uint64_t{123456};
        auto dst = citoq::wire::decode(old_position(), citoq::wire::encode(src));
        assert(dst.get<double>("x") == 10.0);
        assert(dst.get<std::string>("frame") == "odom");
    }

    {
        auto older = old_position();
        citoq::DynamicData src(older);
        src["x"] = 3.0; src["y"] = 4.0; src["frame"] = "map";
        auto dst = citoq::wire::decode(new_position(), citoq::wire::encode(src));
        assert(dst.get<double>("x") == 3.0);
        assert(!dst.has("timestamp"));
    }

    {
        auto renamed = citoq::TypeBuilder("acme.navigation.Position")
            .member<double>(1, "east")
            .member<double>(2, "north")
            .member<std::string>(3, "reference").bound(16)
            .build();
        auto original = old_position();
        citoq::DynamicData src(original);
        src["x"] = 8.0; src["y"] = 9.0; src["frame"] = "map";
        auto dst = citoq::wire::decode(renamed, citoq::wire::encode(src));
        assert(dst.get<double>("east") == 8.0);
        assert(dst.get<std::string>("reference") == "map");
    }

    {
        auto incompatible = citoq::TypeBuilder("acme.navigation.Position")
            .member<std::int32_t>(1, "x")
            .member<double>(2, "y")
            .member<std::string>(3, "frame").bound(16)
            .build();
        auto original = old_position();
        citoq::DynamicData src(original);
        src["x"] = 1.0; src["y"] = 2.0; src["frame"] = "map";
        bool failed = false;
        try { (void)citoq::wire::decode(incompatible, citoq::wire::encode(src)); }
        catch (const citoq::wire::Error&) { failed = true; }
        assert(failed);
    }

    {
        auto other = citoq::TypeBuilder("acme.navigation.Other")
            .member<double>(1, "x")
            .member<double>(2, "y")
            .member<std::string>(3, "frame").bound(16)
            .build();
        auto original = old_position();
        citoq::DynamicData src(original);
        src["x"] = 1.0; src["y"] = 2.0; src["frame"] = "map";
        bool failed = false;
        try { (void)citoq::wire::decode(other, citoq::wire::encode(src)); }
        catch (const citoq::wire::Error&) { failed = true; }
        assert(failed);
    }

    {
        auto type = old_position();
        citoq::DynamicData src(type);
        src["x"] = 1.0; src["y"] = 2.0; src["frame"] = "map";
        auto bytes = citoq::wire::encode(src);
        bytes.pop_back();
        bool failed = false;
        try { (void)citoq::wire::decode(type, bytes); }
        catch (const citoq::wire::Error&) { failed = true; }
        assert(failed);
    }

    return 0;
}
