#!/usr/bin/env python3
"""ctest `config_fresh`: the C++ constants must be the reference config.py's, bit for bit.

Two checks. (1) include/kp/config_gen.hpp must be exactly what gen_config_hpp.py emits today, so a
reference that moved fails here instead of as a mystery mismatch three suites downstream. (2) every
HAND-written constant in config.hpp that shares a name with config.py must hold the same value --
this is the check that would have caught QUOTE_TIER_COUNTS going from three rungs to one.

    python tools/check_config.py <kp-root> <ref-root>
"""
import ast, os, pathlib, re, subprocess, sys

root, ref = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve()
sys.path.insert(0, str(ref))
import config  # noqa: E402

rc = 0
want = subprocess.run([sys.executable, str(root / "tools/gen_config_hpp.py")], check=True,
                      capture_output=True, text=True, env={**os.environ, "KP_REF": str(ref)}).stdout
have = (root / "include/kp/config_gen.hpp").read_text()
if want != have:
    print("STALE include/kp/config_gen.hpp -- regenerate:  make config")
    rc = 1
else:
    print("  config_gen.hpp ok")


def cval(expr):
    e = expr.strip()
    if e in ("true", "false"):
        return e == "true"
    if e.startswith("{"):
        return tuple(cval(x) for x in re.findall(r'"[^"]*"|[^,{}\s][^,{}]*', e))
    if e.startswith('"'):
        return ast.literal_eval(e)
    m = re.fullmatch(r"([0-9.eE+-]+)\s*/\s*([0-9.eE+-]+)", e)
    if m:
        return float(m.group(1)) / float(m.group(2))
    if re.fullmatch(r"[A-Z_][A-Z0-9_]*(\s*[+-]\s*[A-Z_][A-Z0-9_]*)*", e):
        return None                        # defined from other constants; checked through them
    return float.fromhex(e) if "0x" in e else (float(e) if re.search(r"[.eE]", e) else int(e))


n = 0
src = (root / "include/kp/config.hpp").read_text()
for m in re.finditer(r"inline constexpr [^=;]*?\b([A-Z][A-Z0-9_]+)\s*=\s*([^;]+);", src):
    name, expr = m.group(1), m.group(2)
    if name.endswith("_IS_NONE"):
        base = name[: -len("_IS_NONE")]
        if hasattr(config, base) and (getattr(config, base) is None) != cval(expr):
            print(f"  MISMATCH {name}: C++ {expr} vs Python {getattr(config, base)!r}"); rc = 1
        n += 1
        continue
    if not hasattr(config, name):
        continue
    c, p = cval(expr), getattr(config, name)
    if c is None:
        continue
    n += 1
    same = (tuple(c) == tuple(p)) if isinstance(p, tuple) else (
        type(c) is float and type(p) in (float, int) and c.hex() == float(p).hex()) or c == p
    if not same:
        print(f"  MISMATCH {name}: C++ {expr.strip()} vs Python {p!r}"); rc = 1
print(f"  {n} hand-written constants compared")
sys.exit(rc)
