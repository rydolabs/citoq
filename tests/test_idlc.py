import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cito_idlc", ROOT / "tools" / "cito_idlc.py")
mod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)


class IdlcTests(unittest.TestCase):
    def test_complex_subset_maps_to_ast_and_codegen(self):
        text = r'''
        module acme {
          enum Mode { Idle = 0, Active = 2 };
          struct Pose {
            @id(1) double x;
            @id(2) double y;
          };
          struct Telemetry {
            @id(1) Pose pose;
            @id(2) Mode mode;
            @id(3) float coefficients[4];
            @id(4) sequence<string<64>, 20> tags;
            @id(5) @optional unsigned long long timestamp;
          };
        };
        '''
        schema = mod.Parser(text).parse()
        self.assertEqual([e.canonical_name for e in schema.enums], ["acme.Mode"])
        self.assertEqual([s.canonical_name for s in schema.structs], ["acme.Pose", "acme.Telemetry"])
        telemetry = schema.structs[1]
        self.assertEqual(telemetry.fields[2].type_ref.kind, "array")
        self.assertEqual(telemetry.fields[2].type_ref.extent, 4)
        self.assertEqual(telemetry.fields[3].type_ref.kind, "sequence")
        self.assertEqual(telemetry.fields[3].type_ref.bound, 20)
        self.assertEqual(telemetry.fields[3].type_ref.element.kind, "string")
        self.assertEqual(telemetry.fields[3].type_ref.element.bound, 64)
        self.assertTrue(telemetry.fields[4].optional)
        cpp = mod.generate_cpp(schema, "telemetry.idl")
        self.assertIn('TypeBuilder("acme.Telemetry")', cpp)
        self.assertIn('cito::types::array(cito::types::scalar<float>(), 4)', cpp)
        self.assertIn('cito::types::sequence(cito::types::string(64), 20)', cpp)
        self.assertIn('template <> struct StaticCodec<acme::Telemetry>', cpp)
        self.assertIn('static constexpr bool direct = true;', cpp)
        self.assertIn('static_detail::encode_field(', cpp)
        self.assertIn('static_detail::read_field(bytes, pos)', cpp)

    def test_python_codegen_uses_same_schema_model(self):
        schema = mod.Parser("""
        module acme {
          enum Mode { Idle = 0, Active = 2 };
          struct Pose { @id(1) double x; @id(2) double y; };
          struct Telemetry {
            @id(1) Pose pose;
            @id(2) Mode mode;
            @id(3) sequence<string<64>, 20> tags;
            @id(4) @optional unsigned long long timestamp;
          };
        };
        """).parse()
        py = mod.generate_python(schema, "telemetry.idl")
        self.assertIn("class Mode(IntEnum):", py)
        self.assertIn("@dataclasses.dataclass", py)
        self.assertIn("class Telemetry:", py)
        self.assertIn("_wire.structure(_ACME_POSE_SPEC, Pose)", py)
        self.assertIn("_wire.sequence(_wire.string(64), 20)", py)
        self.assertIn("def encode(self) -> bytes:", py)
        self.assertIn("def decode(cls, data:", py)

    def test_request_reply_relation_codegen(self):
        schema = mod.Parser("""
        module acme {
          struct GetStatus {
            @id(1) string<32> drone_id;
          };
          struct Status {
            @id(1) unsigned long battery;
          };
          request GetStatus returns Status;
        };
        """).parse()
        self.assertEqual(len(schema.requests), 1)
        relation = schema.requests[0]
        self.assertEqual(relation.request_type.name, "acme.GetStatus")
        self.assertEqual(relation.reply_type.name, "acme.Status")

        cpp = mod.generate_cpp(schema, "request_reply.idl")
        self.assertIn(
            "template <> struct ReplyType<acme::GetStatus>",
            cpp)
        self.assertIn("using type = acme::Status;", cpp)

        py = mod.generate_python(schema, "request_reply.idl")
        self.assertIn("GetStatus.REPLY_TYPE = Status", py)

    def test_request_reply_requires_structs_and_unique_request(self):
        with self.assertRaisesRegex(mod.IdlError, "request type must be a struct"):
            mod.Parser("""
            enum E { A = 0 };
            struct R { @id(1) long value; };
            request E returns R;
            """).parse()

        with self.assertRaisesRegex(mod.IdlError, "duplicate request reply mapping"):
            mod.Parser("""
            struct Q { @id(1) long value; };
            struct R1 { @id(1) long value; };
            struct R2 { @id(1) long value; };
            request Q returns R1;
            request Q returns R2;
            """).parse()

    def test_requires_explicit_field_id(self):
        with self.assertRaisesRegex(mod.IdlError, "requires explicit @id"):
            mod.Parser("struct P { double x; };").parse()

    def test_rejects_duplicate_field_id(self):
        with self.assertRaisesRegex(mod.IdlError, "duplicate field id"):
            mod.Parser("struct P { @id(1) double x; @id(1) double y; };").parse()

    def test_rejects_unknown_type(self):
        with self.assertRaisesRegex(mod.IdlError, "unknown referenced type"):
            mod.Parser("struct P { @id(1) Missing x; };").parse()

    def test_comments_and_nested_modules(self):
        schema = mod.Parser('''
        // comment
        module a { module b {
          /* block */ struct P { @id(1) long x; };
        }; };
        ''').parse()
        self.assertEqual(schema.structs[0].canonical_name, "a.b.P")

    def test_reorders_structs_by_dependency_for_cpp(self):
        schema = mod.Parser("""
        module a {
          struct Zed { @id(1) double x; };
          struct Alpha { @id(1) Zed z; };
        };
        """).parse()
        cpp = mod.generate_cpp(schema, "deps.idl")
        self.assertLess(cpp.index("struct Zed"), cpp.index("struct Alpha"))

    def test_rejects_duplicate_id_annotation(self):
        with self.assertRaisesRegex(mod.IdlError, "duplicate @id"):
            mod.Parser("struct P { @id(1) @id(2) double x; };").parse()


if __name__ == "__main__":
    unittest.main()
