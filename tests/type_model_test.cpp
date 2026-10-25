#include <citoq/type.hpp>
#include <cassert>
#include <stdexcept>
#include <string>

int main() {
    const auto mode = citoq::EnumBuilder("acme.Mode")
        .value(0, "idle")
        .value(1, "active")
        .build();
    assert(mode.values().size() == 2);
    assert(mode.type_id() != 0);

    const auto pose = citoq::TypeBuilder("acme.Pose")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .build();

    const auto telemetry = citoq::TypeBuilder("acme.Telemetry")
        .member(1, "pose", citoq::types::structure(pose))
        .member(2, "mode", citoq::types::enumeration(mode))
        .member(3, "coefficients", citoq::types::array(citoq::types::scalar<float>(), 4))
        .member(4, "tags", citoq::types::sequence(citoq::types::string(64), 20))
        .member<std::uint64_t>(5, "timestamp").optional()
        .build();

    const auto* pose_field = telemetry.find("pose");
    assert(pose_field && pose_field->type.kind == citoq::TypeKind::Struct);
    assert(pose_field->type.referenced_type_id == pose.type_id());
    assert(pose_field->type.referenced_schema_hash == pose.schema_hash());

    const auto* coeff = telemetry.find("coefficients");
    assert(coeff && coeff->type.kind == citoq::TypeKind::Array);
    assert(coeff->type.extent == 4);
    assert(coeff->type.element && coeff->type.element->kind == citoq::TypeKind::Float32);

    const auto* tags = telemetry.find("tags");
    assert(tags && tags->type.kind == citoq::TypeKind::Sequence);
    assert(tags->type.bound == 20);
    assert(tags->type.element && tags->type.element->kind == citoq::TypeKind::String);
    assert(tags->type.element->bound == 64);

    const auto same = citoq::TypeBuilder("acme.Telemetry")
        .member(1, "pose", citoq::types::structure(pose))
        .member(2, "mode", citoq::types::enumeration(mode))
        .member(3, "coefficients", citoq::types::array(citoq::types::scalar<float>(), 4))
        .member(4, "tags", citoq::types::sequence(citoq::types::string(64), 20))
        .member<std::uint64_t>(5, "timestamp").optional()
        .build();
    assert(telemetry.type_id() == same.type_id());
    assert(telemetry.schema_hash() == same.schema_hash());

    const auto changed_bound = citoq::TypeBuilder("acme.Telemetry")
        .member(1, "pose", citoq::types::structure(pose))
        .member(2, "mode", citoq::types::enumeration(mode))
        .member(3, "coefficients", citoq::types::array(citoq::types::scalar<float>(), 4))
        .member(4, "tags", citoq::types::sequence(citoq::types::string(64), 21))
        .member<std::uint64_t>(5, "timestamp").optional()
        .build();
    assert(telemetry.type_id() == changed_bound.type_id());
    assert(telemetry.schema_hash() != changed_bound.schema_hash());

    bool invalid_array = false;
    try { (void)citoq::types::array(citoq::types::scalar<float>(), 0); }
    catch (const std::invalid_argument&) { invalid_array = true; }
    assert(invalid_array);

    bool duplicate_enum = false;
    try {
        (void)citoq::EnumBuilder("BadEnum").value(1, "a").value(1, "b").build();
    } catch (const std::invalid_argument&) { duplicate_enum = true; }
    assert(duplicate_enum);
}
