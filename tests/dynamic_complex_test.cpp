#include <citoq/wire.hpp>

#include <cassert>
#include <string>

int main() {
    const auto pose = citoq::TypeBuilder("Pose")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .build();

    const auto telemetry = citoq::TypeBuilder("Telemetry")
        .member(1, "pose", citoq::types::structure(pose))
        .member(2, "coefficients",
            citoq::types::array(citoq::types::scalar<float>(), 2))
        .member(3, "tags",
            citoq::types::sequence(citoq::types::string(5), 2))
        .build();

    auto pose_value = std::make_shared<citoq::DynamicData>(pose);
    pose_value->set("x", 1.0);
    pose_value->set("y", 2.0);

    auto coefficients = std::make_shared<citoq::DynamicList>();
    coefficients->values = {1.0f, 2.0f};

    auto tags = std::make_shared<citoq::DynamicList>();
    tags->values = {std::string("a"), std::string("bb")};

    citoq::DynamicData source(telemetry);
    source.set_value("pose", pose_value);
    source.set_value("coefficients", coefficients);
    source.set_value("tags", tags);

    const auto bytes = citoq::wire::encode(source);
    auto decoded = citoq::wire::decode(telemetry, bytes);

    const auto nested =
        std::get<citoq::DynamicStruct>(decoded.value("pose"));
    assert(nested->get<double>("x") == 1.0);
    assert(nested->get<double>("y") == 2.0);

    const auto decoded_coefficients =
        std::get<citoq::DynamicListPtr>(decoded.value("coefficients"));
    assert(decoded_coefficients->values.size() == 2);
    assert(std::get<float>(decoded_coefficients->values[1]) == 2.0f);

    const auto decoded_tags =
        std::get<citoq::DynamicListPtr>(decoded.value("tags"));
    assert(decoded_tags->values.size() == 2);
    assert(std::get<std::string>(decoded_tags->values[1]) == "bb");

    bool sequence_bound_failed = false;
    auto too_many_tags = std::make_shared<citoq::DynamicList>();
    too_many_tags->values = {
        std::string("a"),
        std::string("b"),
        std::string("c")};
    try {
        source.set_value("tags", too_many_tags);
    } catch (const std::length_error&) {
        sequence_bound_failed = true;
    }
    assert(sequence_bound_failed);

    bool array_extent_failed = false;
    auto short_array = std::make_shared<citoq::DynamicList>();
    short_array->values = {1.0f};
    try {
        source.set_value("coefficients", short_array);
    } catch (const std::length_error&) {
        array_extent_failed = true;
    }
    assert(array_extent_failed);

    return 0;
}
