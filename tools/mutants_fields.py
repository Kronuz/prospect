import subprocess,sys,os,json
# usage: [BUILD=dir] mutants_fields.py [LETTER ...]; BUILD (default build-rel) is a configured Release or ASan tree
root=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(root)
BUILD=os.environ.get('BUILD','build-rel')
M=[
 ("A pool threshold 256 -> 255 (D3)","include/prospect/fielddict.hh",[("own_threshold = 256;","own_threshold = 255;")],["fielddict_test","fields_test"]),
 ("B varint accepts a non-canonical long form (D10)","include/prospect/fielddict.hh",[("if (std::uint8_t(p[1]) == 0 || v < 0xF0) { return false; }","")],["fielddict_test"]),
 ("C internal key's varint not order-preserving (D10)","include/prospect/fielddict.hh",[("for (unsigned i = 0; i < n; ++i) { out[1 + i] = char(id >> (8 * (n - 1 - i))); }","for (unsigned i = 0; i < n; ++i) { out[1 + i] = char(id >> (8 * i)); }")],["fielddict_test","fields_test"]),
 ("D stage 1 lets a part name the key field (D7)","include/prospect/reader.hh",[("if (!id || vc.is_key_field(*id)) { return false; }","if (!id) { return false; }")],["fields_test","format13_test"]),
 ("E inverter takes a reference's number for its id (D2)","include/prospect/inverter.hh",[("const auto id = res.resolve(f.field);","const std::optional<std::uint32_t> id = f.field.n;")],["fields_test"]),
 ("F shards sum statistics by id (D14, T5b)","include/prospect/searcher.hh",[("c.infos.push_back(find_term(s, field, term));","{ segment_snapshot t = s; t.fields = segs_.front().fields; c.infos.push_back(find_term(t, field, term)); }")],["shards_test"]),
 ("G key-field terms looked up in the dictionary (D7)","include/prospect/searcher.hh",[("if (s.key_field() != id) {","if (true) {")],["format13_test","fields_test"]),
 ("H reorder merges graph terms by bytes only (D3)","include/prospect/reorder.hh",[("const std::pair<std::uint32_t, std::string_view> at{cursors[s].field(), cursors[s].term()};","const std::pair<std::uint32_t, std::string_view> at{0, cursors[s].term()};"),("cursors[s].field() != current_field || ","")],["reorder_test"]),
 ("I plan counts no terms (writer plan)","include/prospect/fielddict.hh",[("		++plan_.back().terms;\n","")],["fielddict_test","fields_test"]),
 ("J an unknown field name resolves to field 0 (D14)","include/prospect/searcher.hh",[("if (!id) { return std::nullopt; }\n	if (s.key_field() != id)","const std::optional<std::uint32_t> zero = 0;\n	if (!id) { return find_term_zero(s, term); }\n	if (s.key_field() != id)")],[]),
 ("M the deep check ignores an entry's term count (fuzz finding)","include/prospect/fielddict.hh",[('e && e->terms != run) { corrupt("field term count"); }','e && false) {}')],["fielddict_test"]),
 ("L document length ignores the host's (D5)","include/prospect/inverter.hh",[("given, d.length, d.stored","given, std::nullopt, d.stored")],["fields_test"]),
]
F="include/prospect/fielddict.hh"
M+=[
 ("N pooled scan does not stop at the next field (SR2)",F,[("if (id != field) { return false; }","if (false) { return false; }")],["fielddict_oracle_test"]),
 ("O sample search skips a field equal to a sample's (SR2)",F,[("if (sample_id(mid) <= field) { lo = mid; }","if (sample_id(mid) < field) { lo = mid; }")],["fielddict_oracle_test"]),
 ("P pooled scan slices the wrong lead for ids of 240 and up (SR2)",F,[("return f(k.substr(lead), v);","return f(k.substr(lead > 1 ? lead - 1 : lead), v);")],["fielddict_oracle_test"]),
 ("Q pooled lookup under the id field OR 1 (SR2)",F,[("put_field(key, field);\n\t\t\tkey += term;\n\t\t\treturn pool().find_run(key, visit);","put_field(key, field | 1);\n\t\t\tkey += term;\n\t\t\treturn pool().find_run(key, visit);")],["fielddict_oracle_test"]),
 ("R the cursor reads an own dictionary without the verifier (SR1)",F,[("if (!verify_) { return termdict::reader(blocks, index); }","if (true) { return termdict::reader(blocks, index); }")],["fielddict_media_test"]),
 ("S pool blocks without a pool index accepted (SR3)",F,[('		if (pool_index_.empty() != (terms.size() == own_blocks_)) { corrupt("field directory"); }  // pool blocks and pool index come together\n',"")],["fielddict_test"]),
 ("T check_fields lets the key field hold terms (SR5)",F,[("return id < table_size && id != key_field;","return id < table_size;")],["fielddict_test"]),
 ("U check_fields accepts an id equal to the table's size (SR5)",F,[("return id < table_size && id != key_field;","return id <= table_size && id != key_field;")],["fielddict_test"]),
 ("V the key leaf is chosen by the byte Q, not the field's role (F3)","include/prospect/searcher.hh",[("if (s.key_field() != id) {","if (term.empty() || term[0] != 'Q') {")],["format13_test"]),
 ("W the key leaf is chosen by the byte 0xff, not the field's role (F3)","include/prospect/searcher.hh",[("if (s.key_field() != id) {","if (term.empty() || std::uint8_t(term[0]) != 0xff) {")],["format13_test"]),
]
M=[m for m in M if m[3]]
only=sys.argv[1:]
log=open('.scratch/mutants.log','a')
def run(cmd):
    return subprocess.run(cmd,shell=True,capture_output=True,text=True)
for name,f,subs,tests in M:
    if only and name.split()[0] not in only: continue
    src=open(f).read()
    mut=src
    for a,b in subs:
        if a not in mut: print("NO MATCH",name,a[:50]); log.write(f"{name}: NO MATCH\n"); mut=None; break
        mut=mut.replace(a,b,1)
    if mut is None: continue
    open(f,'w').write(mut)
    try:
        b=run(f"cmake --build {BUILD} -j4 --target {' '.join(t.split()[0] for t in tests)} > .scratch/mut-build.log 2>&1")
        err=run("grep -c 'error:' .scratch/mut-build.log").stdout.strip()
        res=[]
        for t in tests:
            r=run(f"cd {BUILD} && ./{t} > {root}/.scratch/mut-run.log 2>&1")
            ok= r.returncode==0
            res.append(f"{t.split()[0]}:{'pass' if ok else 'FAIL'}")
        line=f"{name}: build errors {err}; "+" ".join(res)
    finally:
        open(f,'w').write(src)
    print(line); log.write(line+"\n"); log.flush()
