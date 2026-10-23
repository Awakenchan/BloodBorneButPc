# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import subprocess
import sys

FLOAT4, SINT4, UINT4 = 109, 108, 107
FORMATS = {
    "f": [100, 103, 106, 109],
    "i": [99, 102, 105, 108],
    "u": [98, 101, 104, 107],
}


def base(t):
    if t.startswith(("int", "ivec")):
        return "i"
    if t.startswith(("uint", "uvec")):
        return "u"
    return "f"


def comps(t):
    return int(t[-1]) if t[-1].isdigit() and "vec" in t else 1


def glsl_vec(b, n=4):
    return {"f": "vec", "i": "ivec", "u": "uvec"}[b] + str(n) if n > 1 else {"f": "float", "i": "int", "u": "uint"}[b]


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def bindings(r):
    out = {}

    def add(items, kind_of):
        for it in r.get(items, []):
            if it.get("set", 0) != 0 or "binding" not in it:
                continue
            arr = it.get("array", [1])
            out.setdefault(it["binding"], (kind_of(it), max(1, arr[0] if arr else 1)))

    add("ubos", lambda it: "ub")
    add("ssbos", lambda it: "sb")
    add("separate_images", lambda it: "ut" if "Buffer" in it["type"] else "si")
    add("images", lambda it: "stb" if "Buffer" in it["type"] else "st")
    add("separate_samplers", lambda it: "sa")
    add("textures", lambda it: "cis")
    return ";".join(f"{b}:{k}:{n}" for b, (k, n) in sorted(out.items()))


def partner_vs(inputs, path):
    lines = ["#version 450"]
    body = []
    by_loc = {}
    for i in inputs:
        by_loc.setdefault(i["location"], base(i["type"]))
    for loc, b in sorted(by_loc.items()):
        lines.append(f"layout(location={loc}) out {glsl_vec(b)} o{loc};")
        if b == "f":
            body.append(f"    o{loc} = vec4(float(gl_VertexIndex) * {loc + 1}.25);")
        elif b == "i":
            body.append(f"    o{loc} = ivec4(gl_VertexIndex * {loc + 1});")
        else:
            body.append(f"    o{loc} = uvec4(uint(gl_VertexIndex) * {loc + 1}u);")
    lines.append("void main() {")
    lines += body
    lines.append("    gl_Position = vec4(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1), 0.5, 1.0);")
    lines.append("}")
    open(path, "w").write("\n".join(lines) + "\n")


def partner_fs(outputs, path):
    lines = ["#version 450", "layout(location=0) out vec4 color;"]
    body = ["    vec4 sum = vec4(0.0);"]
    for o in outputs:
        loc, t = o["location"], o["type"]
        if "array" in o:
            continue
        b, n = base(t), comps(t)
        lines.append(f"layout(location={loc}) {'flat ' if b != 'f' else ''}in {glsl_vec(b, n)} i{loc};")
        if b == "f":
            pad = "".join(", 0.0" for _ in range(4 - n))
            body.append(f"    sum += vec4(i{loc}{pad});")
        else:
            body.append(f"    sum += vec4(float(i{loc}{'.x' if n > 1 else ''}));")
    lines.append("void main() {")
    lines += body
    lines.append("    color = sum;")
    lines.append("}")
    open(path, "w").write("\n".join(lines) + "\n")


OWN = {"vert": "VS", "frag": "FS", "comp": "CS"}


def load_report(path):
    """name -> {statistic: value} of the module's own stage, plus create_ms (fastest round)."""
    out = {}
    for line in open(path).read().splitlines()[1:]:
        f = line.split(",")
        if len(f) < 7 or f[5] == "failed":
            continue
        name, stage, _, ms, exe, stat, value = f[:7]
        if not exe.startswith(OWN.get(stage, "?")):
            continue
        m = out.setdefault(name, {"stage": stage, "create_ms": float(ms)})
        m["create_ms"] = min(m["create_ms"], float(ms))
        m[stat] = float(value)
    return out


def compare(a_path, b_path):
    a, b = load_report(a_path), load_report(b_path)
    names = sorted(set(a) & set(b))
    print(f"{len(names)} modules in both ({len(a)} / {len(b)})")
    for stage in ("vert", "frag", "comp"):
        ns = [n for n in names if a[n]["stage"] == stage]
        if not ns:
            continue
        line = [f"{stage} {len(ns)}:"]
        for stat in ("Binary Size", "Register Count", "create_ms"):
            sa = sum(a[n].get(stat, 0) for n in ns)
            sb = sum(b[n].get(stat, 0) for n in ns)
            line.append(f"{stat} {sa:.0f} -> {sb:.0f} ({(sb - sa) / sa * 100 if sa else 0:+.1f}%)")
        changed = sum(1 for n in ns if a[n].get("Binary Size") != b[n].get("Binary Size"))
        line.append(f"binary changed in {changed}")
        print("  ".join(line))
    worst = sorted(names, key=lambda n: b[n].get("Binary Size", 0) - a[n].get("Binary Size", 0))
    for n in worst[:5] + worst[-5:]:
        print(f"  {n} {a[n]['stage']}: binary {a[n].get('Binary Size', 0):.0f} -> {b[n].get('Binary Size', 0):.0f}, "
              f"registers {a[n].get('Register Count', 0):.0f} -> {b[n].get('Register Count', 0):.0f}")


def main():
    if sys.argv[1] == "--compare":
        return compare(sys.argv[2], sys.argv[3])
    spv_dir, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    manifest = []
    skipped = 0
    for name in sorted(os.listdir(spv_dir)):
        if not name.endswith(".spv"):
            continue
        spv = os.path.abspath(os.path.join(spv_dir, name))
        rr = run(["spirv-cross", spv, "--reflect"])
        if rr.returncode != 0:
            skipped += 1
            continue
        r = json.loads(rr.stdout)
        mode = r["entryPoints"][0]["mode"]
        stem = name[:-4]
        vin, cout, partner = "", "", "-"
        if mode in ("vert", "frag"):
            glsl = os.path.join(work, f"{stem}.partner.{'frag' if mode == 'vert' else 'vert'}")
            if mode == "frag":
                partner_vs(r.get("inputs", []), glsl)
                cout = ";".join(f"{o['location']}:{ {'f': FLOAT4, 'i': SINT4, 'u': UINT4}[base(o['type'])] }"
                                for o in r.get("outputs", []))
            else:
                partner_fs(r.get("outputs", []), glsl)
                vin = ";".join(f"{i['location']}:{FORMATS[base(i['type'])][comps(i['type']) - 1]}"
                               for i in r.get("inputs", []))
                cout = f"0:{FLOAT4}"
            partner = os.path.abspath(glsl + ".spv")
            cr = run(["glslangValidator", "-V", "--target-env", "vulkan1.3", glsl, "-o", partner])
            if cr.returncode != 0:
                print(f"{name}: partner failed:\n{cr.stdout}{cr.stderr}")
                skipped += 1
                continue
        elif mode != "comp":
            skipped += 1
            continue
        manifest.append("\t".join([stem, mode, spv, partner, bindings(r), vin, cout]))
    open(os.path.join(work, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
    print(f"{len(manifest)} modules in {os.path.join(work, 'manifest.txt')}, {skipped} left out")


if __name__ == "__main__":
    main()
