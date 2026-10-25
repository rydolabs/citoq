#include <cito/type.hpp>
#include <cassert>

int main() {
    const auto point = cito::TypeBuilder("sim.Point")
        .member<double>(1, "x")
        .member<double>(2, "y")
        .build();

    const auto cloud = cito::TypeBuilder("sim.Cloud")
        .member(1, "points", cito::types::sequence(cito::types::structure(point), 1024))
        .member(2, "frame", cito::types::string(32))
        .build();

    assert(cloud.find("points")->type.kind == cito::TypeKind::Sequence);
    assert(cloud.find("points")->type.element->kind == cito::TypeKind::Struct);
    assert(cloud.find("points")->type.element->referenced_type_id == point.type_id());
    assert(cloud.find("frame")->type.bound == 32);
}
