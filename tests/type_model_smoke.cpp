#include <citoq/type.hpp>
#include <cassert>

int main() {
    const auto point = citoq::TypeBuilder("sim.Point")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .build();

    const auto cloud = citoq::TypeBuilder("sim.Cloud")
        .member(1, "points", citoq::types::sequence(citoq::types::structure(point), 1024))
        .member(2, "frame", citoq::types::string(32))
        .build();

    assert(cloud.find("points")->type.kind == citoq::TypeKind::Sequence);
    assert(cloud.find("points")->type.element->kind == citoq::TypeKind::Struct);
    assert(cloud.find("points")->type.element->referenced_type_id == point.type_id());
    assert(cloud.find("frame")->type.bound == 32);
}
