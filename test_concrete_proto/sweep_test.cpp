// Phase 1 validation sweep: build representative instances, round-trip BINARY (encode -> decode
// into NON-OWNING + pmr arena) and JSON (write_json -> read_json into non-owning + arena).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "solux_types.hpp"
#include "solux.hpp"

using namespace std::string_view_literals;
namespace P = solux::api;

static int bin_pass = 0, bin_fail = 0, json_pass = 0, json_fail = 0;

template <class T>
static bool bin_rt(const T &in, T &out) {
  std::vector<std::byte> wire;
  if (!P::encode(in, wire)) {} // all concrete classes (incl. Hello*) live in solux::api now
  return true;
}

// Use ADL-found encode/decode/write_json/read_json (both demo namespaces declare them).
template <class T>
static void check(const char *name, const T &in, auto &&verify) {
  // BINARY
  {
    std::vector<std::byte> wire;
    bool ok = encode(in, wire);
    std::vector<std::byte> pad = wire; pad.push_back(std::byte{0});
    std::span<const std::byte> payload{pad.data(), pad.size() - 1};
    std::pmr::monotonic_buffer_resource arena;
    T out{};
    ok = ok && decode(out, payload, arena);
    ok = ok && verify(out);
    if (ok) { ++bin_pass; } else { ++bin_fail; printf("  BIN  FAIL %s\n", name); }
  }
  // JSON
  {
    std::string js;
    bool w = write_json(in, js);
    std::pmr::monotonic_buffer_resource arena;
    T out{};
    bool r = w && read_json(out, js, arena);
    bool v = r && verify(out);
    if (v) { ++json_pass; }
    else { ++json_fail; printf("  JSON FAIL %s  [%s]  (json=%s)\n", name,
                                !w?"write":(!r?"read":"verify"), js.c_str()); }
  }
}

int main() {
  // ---------- leaves / simple ----------
  { P::Target m; std::string_view n[]={"a","b"}; m.name=n;
    check("Target", m, [](auto&o){return o.name.size()==2 && o.name[1]=="b"sv;}); }
  { P::RrfFusion m; m.k=60; check("RrfFusion", m, [](auto&o){return o.k==60;}); }
  { P::SortSpec m; m.field="price"; m.dir=P::SortSpec_::SortDir::DESC;
    check("SortSpec", m, [](auto&o){return o.field=="price"sv && o.dir==P::SortSpec_::SortDir::DESC;}); }
  { P::PrefixQuery m; m.field="t"; m.prefix="he";
    check("PrefixQuery", m, [](auto&o){return o.prefix=="he"sv;}); }
  { P::FuzzyQuery m; m.field="t"; m.term="cat"; m.max_edits=2; m.max_expansions=10;
    check("FuzzyQuery", m, [](auto&o){return o.term=="cat"sv && o.max_edits && *o.max_edits==2;}); }
  { P::PhraseQuery m; std::string_view w[]={"a","b"}; std::int32_t p[]={1,2}; m.field="f"; m.words=w; m.positions=p;
    check("PhraseQuery", m, [](auto&o){return o.words.size()==2 && o.positions.size()==2;}); }
  { P::CommitParams m; std::string_view a[]={"vec.*"}; m.commit_within_us=5; m.build_aux_indexes=a; m.wait_for_merges=true;
    check("CommitParams", m, [](auto&o){return o.commit_within_us==5 && o.wait_for_merges;}); }
  { P::ArrStr m; std::string_view v[]={"x","y","z"}; m.v=v; check("ArrStr", m, [](auto&o){return o.v.size()==3;}); }
  { P::ArrInt m; std::int64_t v[]={1,2,3}; m.v=v; check("ArrInt", m, [](auto&o){return o.v.size()==3 && o.v[2]==3;}); }
  { P::ArrFloat m; float v[]={1.5f,2.5f}; m.v=v; check("ArrFloat", m, [](auto&o){return o.v.size()==2;}); }
  { P::ArrDouble m; double v[]={1.5,2.5}; m.v=v; check("ArrDouble", m, [](auto&o){return o.v.size()==2;}); }
  { P::ArrBin m; std::byte b0[]={std::byte{1}}; ::hpp_proto::bytes_view b[]={::hpp_proto::bytes_view(b0)}; m.v=b;
    check("ArrBin", m, [](auto&o){return o.v.size()==1 && o.v[0].size()==1;}); }
  { P::ArrInt32 m; std::int32_t v[]={7,8}; m.v=v; check("ArrInt32", m, [](auto&o){return o.v.size()==2;}); }
  { P::ColStr m; std::string_view v[]={"a","b"}; m.missing_val="~"; m.v=v;
    check("ColStr", m, [](auto&o){return o.v.size()==2 && o.missing_val=="~"sv;}); }
  { P::ColInt m; std::int64_t v[]={9,10}; m.missing_val=-1; m.v=v;
    check("ColInt", m, [](auto&o){return o.v.size()==2 && o.missing_val==-1;}); }
  { P::ColFloat m; float v[]={1.f}; m.missing_val=0.f; m.v=v; check("ColFloat", m, [](auto&o){return o.v.size()==1;}); }
  { P::ColDouble m; double v[]={1.}; m.missing_val=0.; m.v=v; check("ColDouble", m, [](auto&o){return o.v.size()==1;}); }
  { P::AuxIndexInfo m; m.kind="vector_faiss"; m.name="vec.t"; m.gen=3;
    check("AuxIndexInfo", m, [](auto&o){return o.kind=="vector_faiss"sv && o.gen==3;}); }
  { P::AnalyzerDef m; std::string_view f[]={"lowercase"}; m.tokenizer="ws"; m.filters=f;
    check("AnalyzerDef", m, [](auto&o){return o.tokenizer=="ws"sv && o.filters.size()==1;}); }
  { P::VectorParams m; m.dims=128; m.metric=P::VectorParams_::Metric::COSINE; m.normalized=true;
    check("VectorParams", m, [](auto&o){return o.dims==128 && o.metric==P::VectorParams_::Metric::COSINE && o.normalized && *o.normalized;}); }
  { P::FieldDef m; m.name="title"; m.field_class=P::FieldDef_::FieldClass::TEXT; m.indexed=true; m.abstract=false;
    P::AnalyzerDef az; az.tokenizer="ws"; m.analyzer=az;
    check("FieldDef", m, [](auto&o){return o.name=="title"sv && o.field_class && *o.field_class==P::FieldDef_::FieldClass::TEXT && o.indexed && *o.indexed && o.analyzer && o.analyzer->tokenizer=="ws"sv;}); }
  { P::SegmentInfo m; m.seg_id=1; m.max_doc=100; m.live_docs=99;
    check("SegmentInfo", m, [](auto&o){return o.seg_id==1 && o.max_doc==100;}); }
  { P::IndexInfo m; P::SegmentInfo s[1]; s[0].seg_id=5; m.version=2; m.segments=s;
    check("IndexInfo", m, [](auto&o){return o.version==2 && o.segments.size()==1 && o.segments[0].seg_id==5;}); }
  { P::Vector m; P::ArrFloat af; float fv[]={1.f,2.f,3.f}; af.v=fv; m.f32=af;
    check("Vector", m, [](auto&o){return o.f32 && o.f32->v.size()==3;}); }
  { P::KnnQuery m; m.field="emb"; m.k=10; P::Vector v; P::ArrFloat af; float fv[]={1.f}; af.v=fv; v.f32=af; m.query=v;
    check("KnnQuery", m, [](auto&o){return o.field=="emb"sv && o.k==10 && o.query && o.query->f32 && o.query->f32->v.size()==1;}); }
  { P::ArrVector m; P::Vector vs[1]; P::ArrFloat af; float fv[]={1.f}; af.v=fv; vs[0].f32=af; m.v=vs;
    check("ArrVector", m, [](auto&o){return o.v.size()==1;}); }
  { P::ColVector m; P::Vector vs[1]; m.v=vs; check("ColVector", m, [](auto&o){return o.v.size()==1;}); }
  { P::MultiVector m; P::ArrVector av[1]; m.v=av; check("MultiVector", m, [](auto&o){return o.v.size()==1;}); }
  { P::ArrArrStr m; P::ArrStr a[1]; std::string_view sv[]={"q"}; a[0].v=sv; m.v=a;
    check("ArrArrStr", m, [](auto&o){return o.v.size()==1 && o.v[0].v.size()==1;}); }
  { P::ArrArrInt m; P::ArrInt a[1]; std::int64_t iv[]={5}; a[0].v=iv; m.v=a; check("ArrArrInt", m, [](auto&o){return o.v.size()==1;}); }
  { P::ArrArrFloat m; P::ArrFloat a[1]; m.v=a; check("ArrArrFloat", m, [](auto&o){return o.v.size()==1;}); }
  { P::ArrArrDouble m; P::ArrDouble a[1]; m.v=a; check("ArrArrDouble", m, [](auto&o){return o.v.size()==1;}); }
  { P::ArrArrBin m; P::ArrBin a[1]; m.v=a; check("ArrArrBin", m, [](auto&o){return o.v.size()==1;}); }
  { P::SchemaDef m; P::FieldDef f[1]; f[0].name="x"; m.fields=f; check("SchemaDef", m, [](auto&o){return o.fields.size()==1;}); }
  { P::SchemaResponse m; P::SchemaDef sd; m.schema=sd; check("SchemaResponse", m, [](auto&o){return (bool)o.schema;}); }
  { P::SchemaRequest m; m.mode=P::SchemaRequest_::Mode::REPLACE; P::SchemaDef sd; m.schema=sd;
    check("SchemaRequest", m, [](auto&o){return o.mode==P::SchemaRequest_::Mode::REPLACE;}); }

  // ---------- oneofs / recursion ----------
  { P::GenOp m; m.name="op"; P::Val args[1]; args[0].kind=std::string_view("hi"); m.args=args;
    check("GenOp", m, [](auto&o){return o.name=="op"sv && o.args.size()==1;}); }
  { P::ArrVal m; P::Val vs[2]; vs[0].kind=(std::int64_t)7; vs[1].kind=std::string_view("z"); m.v=vs;
    check("ArrVal", m, [](auto&o){return o.v.size()==2 && o.v[0].kind.index()==3;}); }
  { P::ColMap m; P::Map mp[1]; m.v=mp; check("ColMap", m, [](auto&o){return o.v.size()==1;}); }
  // Map holding a Val (indirect)
  { P::Val inner; inner.kind=(std::int64_t)42;
    std::pair<std::string_view, ::hpp_proto::indirect_view<P::Val>> e[]={{"k"sv, &inner}};
    P::Map m; m.fields = P::map_view<std::string_view, ::hpp_proto::indirect_view<P::Val>>(std::span(e));
    check("Map", m, [](auto&o){return o.fields.size()==1 && o.fields.at("k"sv)->kind.index()==3;}); }
  // Column with ColInt arm
  { P::Column m; P::ColInt ci; std::int64_t iv[]={1,2,3}; ci.v=iv; m.kind=ci;
    check("Column", m, [](auto&o){return o.kind.index()==2 && std::get<P::ColInt>(o.kind).v.size()==3;}); }
  { P::ColMap cm; P::Column m; m.kind=cm; check("Column(colmap)", m, [](auto&o){return o.kind.index()==9;}); }
  { P::Columns m; P::Column c; P::ColInt ci; std::int64_t iv[]={1}; ci.v=iv; c.kind=ci;
    std::pair<std::string_view, P::Column> e[]={{"age"sv, c}};
    m.columns = P::map_view<std::string_view, P::Column>(std::span(e));
    check("Columns", m, [](auto&o){return o.columns.size()==1 && o.columns.at("age"sv).kind.index()==2;}); }
  // Val variants
  { P::Val m; m.kind=google::protobuf::NullValue::NULL_VALUE;
    check("Val(null)", m, [](auto&o){return o.kind.index()==1;}); }
  { P::Val m; m.kind=std::string_view("hello");
    check("Val(s)", m, [](auto&o){return o.kind.index()==2 && std::get<std::string_view>(o.kind)=="hello"sv;}); }
  { P::Val m; m.kind=(double)3.14; check("Val(d)", m, [](auto&o){return o.kind.index()==4;}); }
  { P::Val inner; inner.kind=(std::int64_t)1;
    std::pair<std::string_view, ::hpp_proto::indirect_view<P::Val>> e[]={{"a"sv,&inner}};
    P::Map mp; mp.fields=P::map_view<std::string_view, ::hpp_proto::indirect_view<P::Val>>(std::span(e));
    P::Val m; m.kind=mp;
    check("Val(map->val)", m, [](auto&o){return o.kind.index()==8 && std::get<P::Map>(o.kind).fields.size()==1;}); }
  { P::Val arrv; P::Val elems[2]; elems[0].kind=(std::int64_t)1; elems[1].kind=(std::int64_t)2;
    P::ArrVal av; av.v=elems; P::Val m; m.kind=av;
    check("Val(arr)", m, [](auto&o){return o.kind.index()==9 && std::get<P::ArrVal>(o.kind).v.size()==2;}); }

  // ---------- queries ----------
  { P::Match m; m.field="title"; P::Val v; v.kind=std::string_view("cat"); m.val=&v; m.operator_=P::Match_::Operator::AND; m.min_match=1;
    check("Match", m, [](auto&o){return o.field=="title"sv && o.val && o.operator_==P::Match_::Operator::AND;}); }
  { P::BooleanQuery m; P::Query q[2]; q[0].kind=true; q[1].kind=std::string_view("f"); m.required=q; m.min_match=1;
    check("BooleanQuery", m, [](auto&o){return o.required.size()==2 && o.min_match==1;}); }
  { P::ConstantScoreQuery m; P::Query q; q.kind=true; m.query=&q; m.score=0.5f;
    check("ConstantScoreQuery", m, [](auto&o){return o.query && o.score && *o.score==0.5f;}); }
  { P::ForcePrepareQuery m; P::Query q; q.kind=true; m.query=&q;
    check("ForcePrepareQuery", m, [](auto&o){return (bool)o.query;}); }
  { P::Query m; m.kind=true; check("Query(all)", m, [](auto&o){return o.kind.index()==3 && std::get<bool>(o.kind);}); }
  { P::PhraseQuery pq; pq.field="t"; P::Query m; m.kind=pq;
    check("Query(phrase)", m, [](auto&o){return o.kind.index()==5;}); }
  { P::BooleanQuery bq; P::Query sub[1]; sub[0].kind=true; bq.optional=sub; P::Query m; m.kind=bq;
    check("Query(bool->subq)", m, [](auto&o){return o.kind.index()==2 && std::get<P::BooleanQuery>(o.kind).optional.size()==1;}); }
  { P::NamedQuery m; m.name="nq"; P::Query q; q.kind=true; m.query=&q;
    check("NamedQuery", m, [](auto&o){return o.name=="nq"sv && (bool)o.query;}); }

  // ---------- search ops / results ----------
  { P::TopDocs m; P::Query q; q.kind=true; m.query=&q; m.offset=5; m.limit=20; m.get_scores=true;
    std::string_view fl[]={"id"}; m.fields=fl;
    check("TopDocs", m, [](auto&o){return (bool)o.query && o.offset==5 && o.limit && *o.limit==20 && o.fields.size()==1;}); }
  { P::SearchOp m; P::TopDocs td; td.offset=1; m.kind=td;
    check("SearchOp", m, [](auto&o){return o.kind.index()==1 && std::get<P::TopDocs>(o.kind).offset==1;}); }
  { P::FieldFacet m; m.field="cat"; m.limit=10; m.mincount=0; m.missing=true;
    check("FieldFacet", m, [](auto&o){return o.field=="cat"sv && o.limit && *o.limit==10 && o.mincount && *o.mincount==0;}); }
  { P::RangeFacet m; m.field="price"; m.start=0; m.end=100; m.gap=10;
    check("RangeFacet", m, [](auto&o){return o.start && *o.start==0 && o.end && *o.end==100 && o.gap==10;}); }
  { P::Fusion m; P::TopDocs td; td.offset=1;
    std::pair<std::string_view, P::TopDocs> s[]={{"src"sv, td}};
    m.sources=P::map_view<std::string_view, P::TopDocs>(std::span(s)); m.limit=5; P::RrfFusion rf; rf.k=60; m.rrf=rf;
    check("Fusion", m, [](auto&o){return o.sources.size()==1 && o.limit && *o.limit==5 && o.rrf && o.rrf->k==60;}); }
  { P::Domain m; m.op_name="d"; P::Query q; q.kind=true; m.root=&q; std::string_view inc[]={"*"}; m.include_filter=inc;
    check("Domain", m, [](auto&o){return o.op_name=="d"sv && (bool)o.root && o.include_filter.size()==1;}); }
  { P::SearchRequest m; std::byte rid[]={std::byte{7}}; m.request_id=rid;
    P::SearchOp so; P::TopDocs td; td.offset=2; so.kind=td;
    std::pair<std::string_view, ::hpp_proto::indirect_view<P::SearchOp>> e[]={{"q"sv,&so}};
    m.ops=P::map_view<std::string_view, ::hpp_proto::indirect_view<P::SearchOp>>(std::span(e)); m.freshness_us=99;
    check("SearchRequest", m, [](auto&o){return o.request_id.size()==1 && o.ops.size()==1 && o.ops.at("q"sv)->kind.index()==1 && o.freshness_us==99;}); }
  { P::SearchResponse m; P::Val v; v.kind=(std::int64_t)1;
    std::pair<std::string_view, ::hpp_proto::indirect_view<P::Val>> e[]={{"r"sv,&v}};
    m.ops=P::map_view<std::string_view, ::hpp_proto::indirect_view<P::Val>>(std::span(e)); m.error="";
    check("SearchResponse", m, [](auto&o){return o.ops.size()==1 && o.ops.at("r"sv)->kind.index()==3;}); }
  { P::DocList m; m.matches=42; m.max_score=1.5f; m.offset=0;
    P::Column c; P::ColInt ci; std::int64_t iv[]={1,2}; ci.v=iv; c.kind=ci;
    std::pair<std::string_view,P::Column> cc[]={{"age"sv,c}};
    m.columns=P::map_view<std::string_view,P::Column>(std::span(cc));
    check("DocList", m, [](auto&o){return o.matches && *o.matches==42 && o.max_score && o.columns.size()==1;}); }
  { P::FacetResult m; m.total_buckets=3; m.missing=1; std::int64_t cts[]={5,4,3}; m.counts=cts;
    P::Column c; P::ColStr cs; std::string_view vv[]={"a","b","c"}; cs.v=vv; c.kind=cs; m.bucket_ids=c;
    check("FacetResult", m, [](auto&o){return o.total_buckets && *o.total_buckets==3 && o.counts.size()==3 && (bool)o.bucket_ids;}); }
  { P::Bucket m; P::Val v; v.kind=std::string_view("b1"); m.bucket_id=&v;
    check("Bucket", m, [](auto&o){return (bool)o.bucket_id && std::get<std::string_view>(o.bucket_id->kind)=="b1"sv;}); }
  { P::NamedValue m; m.name="nv"; P::Val v; v.kind=(double)2.0; m.val=&v;
    check("NamedValue", m, [](auto&o){return o.name=="nv"sv && (bool)o.val;}); }

  // ---------- update ----------
  { P::UpdateRequest m; std::byte rid[]={std::byte{1}}; m.request_id=rid; m.stream_id=7; m.allow_dups=true;
    std::string_view del[]={"id1","id2"}; m.delete_ids=del; P::CommitParams cp; cp.commit_within_us=0; m.commit=cp;
    check("UpdateRequest", m, [](auto&o){return o.stream_id==7 && o.allow_dups && o.delete_ids.size()==2 && (bool)o.commit;}); }
  { P::UpdateResponse m; std::byte rid[]={std::byte{2}}; m.request_id=rid; m.update_version=5; m.status=P::UpdateResponse_::Status::PARTIAL;
    std::string_view ids[]={"a"}; m.ids=ids;
    P::UpdateResponse_::Error errs[1]; errs[0].id="bad"; errs[0].error_message="oops"; errs[0].index=3; m.errors=errs;
    check("UpdateResponse", m, [](auto&o){return o.update_version==5 && o.status==P::UpdateResponse_::Status::PARTIAL && o.errors.size()==1 && o.errors[0].error_message=="oops"sv;}); }

  // ---------- solux.proto ----------
  { solux::api::HelloRequest m; m.name="yonik"; m.response_count=3; m.async=true;
    check("HelloRequest", m, [](auto&o){return o.name=="yonik"sv && o.response_count==3 && o.async;}); }
  { solux::api::HelloReply m; m.message="hi"; m.response_number=1;
    check("HelloReply", m, [](auto&o){return o.message=="hi"sv && o.response_number==1;}); }

  printf("\n==== SWEEP: BIN %d/%d pass, JSON %d/%d pass ====\n",
         bin_pass, bin_pass+bin_fail, json_pass, json_pass+json_fail);
  return (bin_fail==0) ? 0 : 1;
}
