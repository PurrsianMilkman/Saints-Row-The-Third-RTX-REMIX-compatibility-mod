"""For every cloth pixel shader: which sampler holds what, and which TEXCOORD feeds it.

The shim assumes 'albedo is TEXCOORD0, pattern is TEXCOORD1'. This says, per variant, whether
that is true - by reading the register table and the texld instructions out of the disassembly
rather than assuming either.
"""
import os, re, subprocess, sys

SH   = "sh"
DIS  = r"D:\SR3RTXREMIXCOMP\tools\fxo_disasm.py"
PY   = r"C:\Users\Purrsian\AppData\Local\Programs\Python\Python312\python.exe"
TMP  = "disasm_tmp.txt"

reg_re   = re.compile(r"^//\s+(\S+)\s+(s\d+)\s+\d+\s*$")
texld_re = re.compile(r"^\s+texld\S*\s+(\S+),\s*(\S+),\s*s(\d+)\s*$")
list_re  = re.compile(r"^\s+\[(\d+)\]\s+(ps_3_0|vs_3_0)")

def shaders_in(path):
    out = subprocess.run([PY, DIS, path, "--list"], capture_output=True, text=True).stdout
    return [int(m.group(1)) for ln in out.splitlines()
            for m in [list_re.match(ln)] if m and m.group(2) == "ps_3_0"]

def analyse(path, idx):
    subprocess.run([PY, DIS, path, str(idx), "--out", TMP], capture_output=True, text=True)
    try:
        txt = open(TMP, encoding="latin1").read()
    except OSError:
        return None
    regs = {}
    for ln in txt.splitlines():
        m = reg_re.match(ln)
        if m:
            regs[m.group(2)] = m.group(1)
    uv = {}
    for ln in txt.splitlines():
        m = texld_re.match(ln)
        if m:
            src, smp = m.group(2), "s" + m.group(3)
            # v1.xy / v1 -> TEXCOORD1 ; anything starting with r is computed
            base = src.split(".")[0]
            uv[smp] = ("TEXCOORD" + base[1:]) if base.startswith("v") else "computed"
    return regs, uv, txt

INTEREST = ("Diffuse_MapSampler", "Pattern_MapSampler", "Decal_MapSampler", "Dob_MapSampler",
            "Sphere_MapSampler", "Normal_MapSampler")

rows = []
for f in sorted(os.listdir(SH)):
    if not f.endswith(".fxo_pc"):
        continue
    path = os.path.join(SH, f)
    for idx in shaders_in(path):
        got = analyse(path, idx)
        if not got:
            continue
        regs, uv, txt = got
        if "Pattern_MapSampler" not in regs.values():
            continue
        pat_reg = [r for r, n in regs.items() if n == "Pattern_MapSampler"][0]
        dif_reg = [r for r, n in regs.items() if n == "Diffuse_MapSampler"]
        dif_reg = dif_reg[0] if dif_reg else "-"
        rows.append(dict(file=f.replace(".fxo_pc", ""), idx=idx,
                         pat=pat_reg, pat_uv=uv.get(pat_reg, "not sampled"),
                         dif=dif_reg, dif_uv=uv.get(dif_reg, "not sampled"),
                         clamp=("ClampU1" in txt), regs=regs, uv=uv))

print("%-26s %-4s %-18s %-24s %s" % ("shader", "ps", "Pattern_Map", "Diffuse_Map", "clamp window"))
print("-" * 100)
for r in rows:
    print("%-26s [%d]  %-4s <- %-11s %-4s <- %-15s %s" % (
        r["file"], r["idx"], r["pat"], r["pat_uv"], r["dif"], r["dif_uv"],
        "YES ClampU1/V1/U2/V2" if r["clamp"] else ""))

print()
print("VARIANTS WHERE THE PATTERN IS AT s0 (which is what the underwear's draw reports):")
for r in rows:
    if r["pat"] == "s0":
        print("   %s [%d]" % (r["file"], r["idx"]))
        for sreg in sorted(r["regs"], key=lambda x: int(x[1:])):
            print("        %-4s %-28s <- %s" % (sreg, r["regs"][sreg],
                                                r["uv"].get(sreg, "not sampled")))
