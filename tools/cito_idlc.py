#!/usr/bin/env python3
from __future__ import annotations
import argparse, dataclasses, pathlib, re, sys
from typing import Optional

class IdlError(Exception): pass
@dataclasses.dataclass(frozen=True)
class TypeRef:
    kind:str; name:str=""; bound:int=0; extent:int=0; element:Optional['TypeRef']=None
@dataclasses.dataclass
class Field: field_id:int; name:str; type_ref:TypeRef; optional:bool=False
@dataclasses.dataclass
class StructDef:
    name:str; modules:tuple[str,...]; fields:list[Field]
    @property
    def canonical_name(self): return ".".join((*self.modules,self.name))
    @property
    def cpp_name(self): return "::".join((*self.modules,self.name))
@dataclasses.dataclass
class EnumValue: name:str; value:int
@dataclasses.dataclass
class EnumDef:
    name:str; modules:tuple[str,...]; values:list[EnumValue]
    @property
    def canonical_name(self): return ".".join((*self.modules,self.name))
    @property
    def cpp_name(self): return "::".join((*self.modules,self.name))
@dataclasses.dataclass
class RequestDef:
    request_type:TypeRef; reply_type:TypeRef; modules:tuple[str,...]
@dataclasses.dataclass
class Schema:
    structs:list[StructDef]
    enums:list[EnumDef]
    requests:list[RequestDef]=dataclasses.field(default_factory=list)
@dataclasses.dataclass(frozen=True)
class Token: text:str; pos:int

TOKEN=re.compile(r'(?P<W>\s+)|(?P<L>//[^\n]*)|(?P<B>/\*.*?\*/)|(?P<N>-?\d+)|(?P<S>::)|(?P<I>[A-Za-z_]\w*)|(?P<P>[@{}();,<>=\[\]])',re.S)
def tokenize(text):
    out=[]; p=0
    while p<len(text):
        m=TOKEN.match(text,p)
        if not m: raise IdlError(f"line {text.count(chr(10),0,p)+1}: unsupported token {text[p:p+20]!r}")
        if m.lastgroup not in {'W','L','B'}: out.append(Token(m.group(),p))
        p=m.end()
    return out+[Token('<eof>',len(text))]

class Parser:
    def __init__(self,text): self.text=text; self.t=tokenize(text); self.i=0; self.structs=[]; self.enums=[]; self.requests=[]; self.names={}
    def err(self,msg,t=None):
        t=t or self.t[self.i]; return IdlError(f"line {self.text.count(chr(10),0,t.pos)+1}: {msg} (got {t.text!r})")
    def at(self,x): return self.t[self.i].text==x
    def pop(self): x=self.t[self.i]; self.i+=1; return x
    def need(self,x):
        if not self.at(x): raise self.err(f"expected {x!r}")
        return self.pop()
    def ident(self):
        x=self.t[self.i]
        if not re.fullmatch(r'[A-Za-z_]\w*',x.text): raise self.err('expected identifier',x)
        self.i+=1; return x.text
    def positive(self,what):
        x=self.pop()
        if not re.fullmatch(r'\d+',x.text) or int(x.text)<=0: raise self.err(f'{what} must be a positive integer',x)
        return int(x.text)
    def parse(self): self.defs(()); self.need('<eof>'); self.resolve(); return Schema(self.structs,self.enums,self.requests)
    def defs(self,mods,stop=False):
        while not self.at('<eof>') and not(stop and self.at('}')):
            if self.at('module'): self.module(mods)
            elif self.at('struct'): self.struct(mods)
            elif self.at('enum'): self.enum(mods)
            elif self.at('request'): self.request(mods)
            else: raise self.err('expected module, struct, enum, or request relation')
    def module(self,mods):
        self.need('module'); n=self.ident(); self.need('{'); self.defs((*mods,n),True); self.need('}'); self.need(';')
    def register(self,mods,n,k):
        q='.'.join((*mods,n))
        if q in self.names: raise self.err(f'duplicate type declaration {q}')
        self.names[q]=k
    def enum(self,mods):
        self.need('enum'); n=self.ident(); self.register(mods,n,'enum'); self.need('{'); vals=[]; names=set(); nums=set(); cur=0
        while not self.at('}'):
            vn=self.ident()
            if vn in names: raise self.err(f'duplicate enum value name {vn}')
            if self.at('='):
                self.pop(); x=self.pop()
                if not re.fullmatch(r'-?\d+',x.text): raise self.err('expected enum integer value',x)
                cur=int(x.text)
            if cur in nums: raise self.err(f'duplicate enum numeric value {cur}')
            vals.append(EnumValue(vn,cur)); names.add(vn); nums.add(cur); cur+=1
            if self.at(','): self.pop()
            else: break
        self.need('}'); self.need(';'); self.enums.append(EnumDef(n,mods,vals))
    def request(self,mods):
        self.need('request')
        request_type=self.type(mods)
        if request_type.kind!='named':
            raise self.err('request relation requires a named struct request type')
        self.need('returns')
        reply_type=self.type(mods)
        if reply_type.kind!='named':
            raise self.err('request relation requires a named struct reply type')
        self.need(';')
        self.requests.append(RequestDef(request_type,reply_type,mods))
    def struct(self,mods):
        self.need('struct'); n=self.ident(); self.register(mods,n,'struct'); self.need('{'); fs=[]; ids=set(); names=set()
        while not self.at('}'):
            fid=None; opt=False
            while self.at('@'):
                self.pop(); a=self.ident()
                if a=='id':
                    if fid is not None: raise self.err('duplicate @id annotation')
                    self.need('('); fid=self.positive('field id'); self.need(')')
                elif a=='optional':
                    if opt: raise self.err('duplicate @optional annotation')
                    if self.at('('): self.pop(); self.need(')')
                    opt=True
                else: raise self.err(f'unsupported Cito IDL annotation @{a}')
            if fid is None: raise self.err('every Cito struct member requires explicit @id(N)')
            typ=self.type(mods); fn=self.ident(); dims=[]
            while self.at('['): self.pop(); dims.append(self.positive('array extent')); self.need(']')
            for d in reversed(dims): typ=TypeRef('array',extent=d,element=typ)
            self.need(';')
            if fid in ids: raise self.err(f'duplicate field id {fid}')
            if fn in names: raise self.err(f'duplicate field name {fn}')
            ids.add(fid); names.add(fn); fs.append(Field(fid,fn,typ,opt))
        self.need('}'); self.need(';'); self.structs.append(StructDef(n,mods,fs))
    def type(self,mods):
        if self.at('boolean'): self.pop(); return TypeRef('bool')
        if self.at('float'): self.pop(); return TypeRef('float32')
        if self.at('double'): self.pop(); return TypeRef('float64')
        if self.at('long'):
            self.pop()
            if self.at('long'): self.pop(); return TypeRef('int64')
            return TypeRef('int32')
        if self.at('unsigned'):
            self.pop(); self.need('long')
            if self.at('long'): self.pop(); return TypeRef('uint64')
            return TypeRef('uint32')
        if self.at('string'):
            self.pop(); b=0
            if self.at('<'): self.pop(); b=self.positive('string bound'); self.need('>')
            return TypeRef('string',bound=b)
        if self.at('sequence'):
            self.pop(); self.need('<'); e=self.type(mods); b=0
            if self.at(','): self.pop(); b=self.positive('sequence bound')
            self.need('>'); return TypeRef('sequence',bound=b,element=e)
        parts=[self.ident()]
        while self.at('::'): self.pop(); parts.append(self.ident())
        raw='.'.join(parts); return TypeRef('named',name=raw if len(parts)>1 else '.'.join((*mods,raw)))
    def resolve(self):
        def one(r,mods):
            if r.kind in {'array','sequence'}: one(r.element,mods); return
            if r.kind!='named' or r.name in self.names: return
            leaf=r.name.rsplit('.',1)[-1]
            for d in range(len(mods),-1,-1):
                q='.'.join((*mods[:d],leaf)) if d else leaf
                if q in self.names: object.__setattr__(r,'name',q); return
            raise IdlError(f'unknown referenced type {r.name}')
        for s in self.structs:
            for f in s.fields: one(f.type_ref,s.modules)
        seen_requests=set()
        for r in self.requests:
            one(r.request_type,r.modules)
            one(r.reply_type,r.modules)
            if self.names.get(r.request_type.name)!='struct':
                raise IdlError(f'request type must be a struct: {r.request_type.name}')
            if self.names.get(r.reply_type.name)!='struct':
                raise IdlError(f'reply type must be a struct: {r.reply_type.name}')
            if r.request_type.name in seen_requests:
                raise IdlError(f'duplicate request reply mapping for {r.request_type.name}')
            seen_requests.add(r.request_type.name)

CPP={'bool':'bool','int32':'std::int32_t','uint32':'std::uint32_t','int64':'std::int64_t','uint64':'std::uint64_t','float32':'float','float64':'double','string':'std::string'}
SPEC={'bool':'cito::types::scalar<bool>()','int32':'cito::types::scalar<std::int32_t>()','uint32':'cito::types::scalar<std::uint32_t>()','int64':'cito::types::scalar<std::int64_t>()','uint64':'cito::types::scalar<std::uint64_t>()','float32':'cito::types::scalar<float>()','float64':'cito::types::scalar<double>()'}
def qcpp(n): return '::'.join(n.split('.'))
def cpp_type(r):
    if r.kind in CPP:return CPP[r.kind]
    if r.kind=='named':return qcpp(r.name)
    if r.kind=='array':return f'std::array<{cpp_type(r.element)}, {r.extent}>'
    if r.kind=='sequence':return f'std::vector<{cpp_type(r.element)}>'
    raise IdlError(f'cannot map C++ type {r.kind}')
def spec_expr(r,kinds):
    if r.kind in SPEC:return SPEC[r.kind]
    if r.kind=='string':return f'cito::types::string({r.bound})' if r.bound else 'cito::types::string()'
    if r.kind=='array':return f'cito::types::array({spec_expr(r.element,kinds)}, {r.extent})'
    if r.kind=='sequence':return f'cito::types::sequence({spec_expr(r.element,kinds)}, {r.bound})' if r.bound else f'cito::types::sequence({spec_expr(r.element,kinds)})'
    if r.kind=='named':
        c=qcpp(r.name); return f'cito::types::structure(cito::StaticType<{c}>::type())' if kinds[r.name]=='struct' else f'cito::types::enumeration(cito::StaticEnum<{c}>::type())'
    raise IdlError(f'cannot map type spec {r.kind}')
def dyn_ok(r,kinds): return r.kind in CPP or (r.kind=='named' and kinds.get(r.name)=='enum')
def namespaces(mods,open_=True): return [f'namespace {m} {{' for m in mods] if open_ else [f'}} // namespace {m}' for m in reversed(mods)]
def generate_cpp(schema,source_name='schema.idl'):
    kinds={e.canonical_name:'enum' for e in schema.enums}|{s.canonical_name:'struct' for s in schema.structs}
    def deps(r):
        if r.kind in {'array','sequence'}: return deps(r.element)
        return {r.name} if r.kind=='named' and kinds.get(r.name)=='struct' else set()
    pending=list(schema.structs); ordered=[]; done=set()
    while pending:
        moved=False
        for s in list(pending):
            d=set().union(*(deps(f.type_ref) for f in s.fields)); d.discard(s.canonical_name)
            if d<=done: ordered.append(s); done.add(s.canonical_name); pending.remove(s); moved=True
        if not moved: raise IdlError('cyclic or unresolved struct dependency: '+', '.join(s.canonical_name for s in pending))
    out=['#pragma once',f'// Generated by cito_idlc.py from {source_name}. Do not edit.','#include <array>','#include <cstdint>','#include <optional>','#include <string>','#include <vector>','#include <cito/static.hpp>','']
    for d in [*schema.enums,*ordered]:
        out+=namespaces(d.modules)
        if isinstance(d,EnumDef): out += [f'enum class {d.name} : std::int32_t {{',*[f'    {v.name} = {v.value},' for v in d.values],'};']
        else:
            out.append(f'struct {d.name} {{')
            for f in d.fields:
                t=cpp_type(f.type_ref); t=f'std::optional<{t}>' if f.optional else t; out.append(f'    {t} {f.name}{{}};')
            out.append('};')
        out+=namespaces(d.modules,False)+['']
    out+=['namespace cito {']
    for e in schema.enums:
        out += [f'template <> struct StaticEnum<{e.cpp_name}> {{','    static EnumType type() {',f'        auto b = EnumBuilder("{e.canonical_name}");',*[f'        b.value({v.value}, "{v.name}");' for v in e.values],'        return b.build();','    }','};','']
    for s in ordered:
        out += [
            f'template <> struct StaticType<{s.cpp_name}> {{',
            '    static Type type() {',
            f'        auto b = TypeBuilder("{s.canonical_name}");'
        ]
        for f in s.fields:
            out.append(
                f'        b.member({f.field_id}, "{f.name}", {spec_expr(f.type_ref,kinds)});')
            if f.optional:
                out.append('        b.optional();')
        out += [
            '        return b.build();',
            '    }',
            f'    static DynamicData to_dynamic(const {s.cpp_name}& value) {{',
            '        DynamicData data(type());'
        ]
        for f in s.fields:
            if f.optional:
                out.append(
                    f'        if (value.{f.name}) data.set_value("{f.name}", '
                    f'cito::to_dynamic_value(*value.{f.name}));')
            else:
                out.append(
                    f'        data.set_value("{f.name}", '
                    f'cito::to_dynamic_value(value.{f.name}));')
        out += [
            '        return data;',
            '    }',
            f'    static {s.cpp_name} from_dynamic(const DynamicData& data) {{',
            f'        {s.cpp_name} value{{}};'
        ]
        for f in s.fields:
            cpp = cpp_type(f.type_ref)
            if f.optional:
                out.append(
                    f'        if (data.has("{f.name}")) value.{f.name} = '
                    f'cito::from_dynamic_value<{cpp}>(data.value("{f.name}"));')
            else:
                out.append(
                    f'        value.{f.name} = '
                    f'cito::from_dynamic_value<{cpp}>(data.value("{f.name}"));')
        out += [
            '        return value;',
            '    }',
            '};',
            ''
        ]

        required_count = sum(1 for f in s.fields if not f.optional)
        out += [
            f'template <> struct StaticCodec<{s.cpp_name}> {{',
            '    static constexpr bool direct = true;',
            f'    static void encode_into(std::vector<std::uint8_t>& out, const {s.cpp_name}& value) {{',
            f'        const auto& schema = static_type_ref<{s.cpp_name}>();',
            f'        std::uint32_t field_count = {required_count};'
        ]
        for f in s.fields:
            if f.optional:
                out.append(f'        if (value.{f.name}) ++field_count;')
        out.append('        static_detail::write_header(out, schema, field_count);')
        for index, f in enumerate(s.fields):
            if f.optional:
                out.append(
                    f'        if (value.{f.name}) static_detail::encode_field('
                    f'out, schema.fields()[{index}], *value.{f.name});')
            else:
                out.append(
                    f'        static_detail::encode_field('
                    f'out, schema.fields()[{index}], value.{f.name});')
        out += [
            '    }',
            f'    static std::vector<std::uint8_t> encode(const {s.cpp_name}& value) {{',
            '        std::vector<std::uint8_t> out;',
            '        out.reserve(128);',
            '        encode_into(out, value);',
            '        return out;',
            '    }',
            f'    static {s.cpp_name} decode(std::span<const std::uint8_t> bytes) {{',
            f'        const auto& schema = static_type_ref<{s.cpp_name}>();',
            '        const auto header = static_detail::begin_decode(schema, bytes);',
            f'        {s.cpp_name} value{{}};',
            f'        std::array<bool, {len(s.fields)}> seen{{}};',
            '        std::size_t pos = 24;',
            '        for (std::uint32_t i = 0; i < header.field_count; ++i) {',
            '            const auto encoded = static_detail::read_field(bytes, pos);',
            '            switch (encoded.id) {'
        ]
        for index, f in enumerate(s.fields):
            cpp = cpp_type(f.type_ref)
            out += [
                f'                case {f.field_id}:',
                f'                    static_detail::accept_field(encoded, schema.fields()[{index}], seen[{index}]);',
                f'                    value.{f.name} = static_detail::decode_value<{cpp}>(',
                f'                        schema.fields()[{index}].type, encoded.payload);',
                '                    break;'
            ]
        out += [
            '                default:',
            '                    break;',
            '            }',
            '        }',
            '        static_detail::finish_decode(bytes, pos);'
        ]
        for index, f in enumerate(s.fields):
            if not f.optional:
                out.append(
                    f'        static_detail::require_present(seen[{index}], "{f.name}");')
        out += [
            '        return value;',
            '    }',
            '};',
            ''
        ]
    for r in schema.requests:
        out += [
            f'template <> struct ReplyType<{qcpp(r.request_type.name)}> {{',
            f'    using type = {qcpp(r.reply_type.name)};',
            '};',
            ''
        ]
    return '\n'.join(out+['} // namespace cito',''])


PY_SCALAR={'bool':'bool','int32':'int','uint32':'int','int64':'int','uint64':'int','float32':'float','float64':'float','string':'str'}
PY_KIND={'bool':'_wire.BOOL','int32':'_wire.INT32','uint32':'_wire.UINT32','int64':'_wire.INT64','uint64':'_wire.UINT64','float32':'_wire.FLOAT32','float64':'_wire.FLOAT64'}

def py_leaf(name): return name.rsplit('.',1)[-1]
def py_spec_name(name): return '_'+name.replace('.','_').upper()+'_SPEC'
def py_type(r):
    if r.kind in PY_SCALAR:return PY_SCALAR[r.kind]
    if r.kind=='named':return py_leaf(r.name)
    if r.kind in {'array','sequence'}:return f'list[{py_type(r.element)}]'
    raise IdlError(f'cannot map Python type {r.kind}')
def ordered_structs(schema):
    kinds={e.canonical_name:'enum' for e in schema.enums}|{s.canonical_name:'struct' for s in schema.structs}
    def deps(r):
        if r.kind in {'array','sequence'}:return deps(r.element)
        return {r.name} if r.kind=='named' and kinds.get(r.name)=='struct' else set()
    pending=list(schema.structs); ordered=[]; done=set()
    while pending:
        moved=False
        for s in list(pending):
            d=set().union(*(deps(f.type_ref) for f in s.fields)); d.discard(s.canonical_name)
            if d<=done: ordered.append(s); done.add(s.canonical_name); pending.remove(s); moved=True
        if not moved: raise IdlError('cyclic or unresolved struct dependency: '+', '.join(s.canonical_name for s in pending))
    return ordered
def py_default(r,kinds,enums):
    if r.kind=='bool':return 'False'
    if r.kind in {'int32','uint32','int64','uint64'}:return '0'
    if r.kind in {'float32','float64'}:return '0.0'
    if r.kind=='string':return "''"
    if r.kind=='named':
        leaf=py_leaf(r.name)
        if kinds[r.name]=='enum':return f'{leaf}.{enums[r.name].values[0].name}'
        return f'{leaf}()'
    if r.kind=='sequence':return '[]'
    if r.kind=='array':return f'[{py_default(r.element,kinds,enums)} for _ in range({r.extent})]'
    raise IdlError(f'cannot create Python default for {r.kind}')
def py_spec_expr(r,kinds):
    if r.kind in PY_KIND:return f'_wire.scalar({PY_KIND[r.kind]})'
    if r.kind=='string':return f'_wire.string({r.bound})' if r.bound else '_wire.string()'
    if r.kind=='named':
        leaf=py_leaf(r.name)
        return f'_wire.structure({py_spec_name(r.name)}, {leaf})' if kinds[r.name]=='struct' else f'_wire.enumeration({py_spec_name(r.name)}, {leaf})'
    if r.kind=='array':return f'_wire.array({py_spec_expr(r.element,kinds)}, {r.extent})'
    if r.kind=='sequence':
        e=py_spec_expr(r.element,kinds)
        return f'_wire.sequence({e}, {r.bound})' if r.bound else f'_wire.sequence({e})'
    raise IdlError(f'cannot map Python type spec {r.kind}')
def generate_python(schema,source_name='schema.idl'):
    kinds={e.canonical_name:'enum' for e in schema.enums}|{s.canonical_name:'struct' for s in schema.structs}
    enums={e.canonical_name:e for e in schema.enums}
    leaves=[d.name for d in [*schema.enums,*schema.structs]]
    if len(set(leaves))!=len(leaves):
        raise IdlError('Python generator requires unique type leaf names within one IDL file')
    out=['from __future__ import annotations',f'# Generated by cito_idlc.py from {source_name}. Do not edit.','import dataclasses','from enum import IntEnum','from cito import _wire','']
    for e in schema.enums:
        out += [f'class {e.name}(IntEnum):',*[f'    {v.name} = {v.value}' for v in e.values],'']
        out += [f'{py_spec_name(e.canonical_name)} = _wire.enum_spec(',f'    "{e.canonical_name}", {e.name},','    (',*[f'        ("{v.name}", {v.value}),' for v in e.values],'    ),',')','']
    for s in ordered_structs(schema):
        out += ['@dataclasses.dataclass',f'class {s.name}:']
        if not s.fields: out.append('    pass')
        for f in s.fields:
            t=py_type(f.type_ref)
            if f.optional:
                out.append(f'    {f.name}: {t} | None = None')
            elif f.type_ref.kind in {'array','sequence'} or (f.type_ref.kind=='named' and kinds[f.type_ref.name]=='struct'):
                out.append(f'    {f.name}: {t} = dataclasses.field(default_factory=lambda: {py_default(f.type_ref,kinds,enums)})')
            else:
                out.append(f'    {f.name}: {t} = {py_default(f.type_ref,kinds,enums)}')
        out += ['', '    def encode(self) -> bytes:',f'        return _wire.encode_struct({py_spec_name(s.canonical_name)}, self)','', '    @classmethod','    def decode(cls, data: bytes | bytearray | memoryview):',f'        return _wire.decode_struct(cls, {py_spec_name(s.canonical_name)}, data)','']
        out += [f'{py_spec_name(s.canonical_name)} = _wire.struct_spec(',f'    "{s.canonical_name}",','    (']
        for f in s.fields:
            out.append(f'        _wire.FieldSpec({f.field_id}, "{f.name}", {py_spec_expr(f.type_ref,kinds)}, {f.optional}),')
        out += ['    ),',')',f'{s.name}.TYPE_ID = {py_spec_name(s.canonical_name)}.type_id',f'{s.name}.SCHEMA_HASH = {py_spec_name(s.canonical_name)}.schema_hash','']
    for r in schema.requests:
        out += [
            f'{py_leaf(r.request_type.name)}.REPLY_TYPE = {py_leaf(r.reply_type.name)}',
            ''
        ]
    return '\n'.join(out)

def main(argv=None):
    ap=argparse.ArgumentParser(description='Cito OMG-IDL subset compiler'); ap.add_argument('input',type=pathlib.Path); ap.add_argument('-o','--output',type=pathlib.Path,required=True); ap.add_argument('--lang',choices=('cpp','python'),default='cpp'); a=ap.parse_args(argv)
    try:
        schema=Parser(a.input.read_text(encoding='utf-8')).parse()
        generated=generate_cpp(schema,a.input.name) if a.lang=='cpp' else generate_python(schema,a.input.name)
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(generated,encoding='utf-8')
    except (OSError,IdlError) as e: print(f'cito-idlc: {e}',file=sys.stderr); return 2
    return 0
if __name__=='__main__': raise SystemExit(main())
