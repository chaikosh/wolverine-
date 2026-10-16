"""Recompiler driver: python recomp/main.py <exe> <outdir> [--analyze-only]"""
import sys, os, time, pickle, collections
sys.setrecursionlimit(10000)
from pe import PE
from analyze import Analyzer


def main():
    exe, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    pe = PE(exe)
    an = Analyzer(pe)
    print(f"[analyze] {os.path.basename(exe)} entry={pe.entry:#x}")
    funcs = an.run()
    ninsn = sum(len(f.insns) for f in funcs.values())
    print(f"[analyze] {len(funcs)} functions, {ninsn} instructions, {time.time()-t0:.1f}s")
    cov = an.coverage()
    total = len(cov)
    code_b = cov.count(1); data_b = cov.count(2)
    pad = sum(1 for i, c in enumerate(cov) if c == 0 and pe.u8(an.code[0].va + i) in (0xCC, 0x90))
    print(f"[coverage] code {code_b/total:.1%}  tables {data_b/total:.1%}  padding {pad/total:.1%}  "
          f"unknown {(total-code_b-data_b-pad)/total:.1%}")
    jt = sum(len(f.jumptables) for f in funcs.values())
    ij = sum(len(f.indirect_jumps) for f in funcs.values())
    bad = sum(len(f.bad) for f in funcs.values())
    tc = sum(len(f.tailcalls) for f in funcs.values())
    fi = sum(len(f.fallinto) for f in funcs.values())
    print(f"[cfg] jump tables {jt}, unresolved indirect jmps {ij}, tailcalls {tc}, fall-into {fi}, decode failures {bad}")
    origins = collections.Counter(an.seed_origin[a][0] for a in funcs)
    print(f"[seeds] by origin: {dict(origins)}")
    mn = collections.Counter(an.cache[a].mn for f in funcs.values() for a in f.insns)
    with open(os.path.join(out, "mnemonics.txt"), "w") as fh:
        for k, v in mn.most_common():
            fh.write(f"{v:8d} {k}\n")
    print(f"[isa] {len(mn)} distinct mnemonics (see {out}/mnemonics.txt)")
    with open(os.path.join(out, "analysis.pkl"), "wb") as fh:
        pickle.dump({"funcs": funcs, "cache": {a: an.cache[a] for f in funcs.values() for a in f.insns},
                     "data_ranges": an.data_ranges, "seed_origin": an.seed_origin}, fh)
    if "--analyze-only" in sys.argv:
        return
    from lift import emit_all
    emit_all(pe, an, out)
    print(f"[done] {time.time()-t0:.1f}s")


if __name__ == "__main__":
    main()
