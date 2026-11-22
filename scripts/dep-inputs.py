#!/usr/bin/env python3
"""Prints the identity of everything an AROS dependency build depends on.

    scripts/dep-inputs.py ROOT TARGET CC AR SDK OPENSSL_TARBALL JENT_INPUT [OPTION...]

One line per input. The SDK is identified by the SHA-256 of every file
under include/ and lib/, following symbolic links (a linked file by its
target's content, a linked directory by walking it, each real directory
once). The compiler by its driver file, the version it reports, every
program it runs to compile and link (from `-###`), the libgcc/builtins
archive it names and toolchain libraries it links from outside the SDK.
Paths inside the repository are written relative to it.

Limits: programs that those programs start in turn (a linker plugin, for
example) and libraries they load are not identified; a driver that does
not answer `-###` is identified by its file and version only.
"""
import hashlib
import os
import shlex
import subprocess
import sys
import tempfile


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tree(root):
    h = hashlib.sha256()
    count = 0
    for sub in ("include", "lib"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            raise SystemExit(f"SDK has no {sub}/: {root}")
        seen = set()
        for d, dirs, files in os.walk(base, followlinks=True):
            real = os.path.realpath(d)
            if real in seen:  # a link back into the tree: walked once
                dirs[:] = []
                continue
            seen.add(real)
            dirs.sort()
            for name in sorted(files):
                p = os.path.join(d, name)
                rel = os.path.relpath(p, root)
                if os.path.isfile(p):  # follows links
                    h.update(f"{rel} {sha(p)}\n".encode())
                    count += 1
                else:
                    target = os.readlink(p) if os.path.islink(p) else "?"
                    h.update(f"{rel} unresolved {target}\n".encode())
    return h.hexdigest(), count


def tool(path):
    real = os.path.realpath(path)
    try:
        ver = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=60).stdout.splitlines()
    except OSError as e:
        raise SystemExit(f"cannot run {path}: {e}")
    return f"{path} -> {real} sha256 {sha(real)} version {ver[0] if ver else '?'}"


def behind(cc, sdk):
    """Programs and toolchain libraries the driver uses, from -###."""
    out = []
    with tempfile.TemporaryDirectory(prefix="dep-inputs-") as tmp:
        src = os.path.join(tmp, "x.c")
        obj = os.path.join(tmp, "x.o")
        open(src, "w").write("int x;\n")
        open(obj, "w").write("")
        lines = []
        for args in (["-###", "-c", src, "-o", obj], ["-###", obj, "-o", os.path.join(tmp, "x")]):
            try:
                # Asked exactly as the builds call it: with --sysroot, or the
                # driver reports its built-in sysroot's libraries instead.
                r = subprocess.run([cc, "--sysroot=" + sdk] + args, capture_output=True, text=True, timeout=60)
            except OSError:
                continue
            lines += (r.stderr + r.stdout).splitlines()
        progs, libdirs, libs = [], [], []
        for line in lines:
            line = line.strip()
            if not line or not (line.startswith('"') or line.startswith("/")):
                continue
            try:
                words = shlex.split(line)
            except ValueError:
                continue
            if words and os.path.isfile(words[0]):
                progs.append(words[0])
            for w in words[1:]:
                if w.startswith("-L") and len(w) > 2:
                    libdirs.append(w[2:])
                elif w.startswith("-l") and len(w) > 2:
                    libs.append(w[2:])
                elif os.path.isabs(w) and w.endswith((".a", ".o")) and os.path.isfile(w) \
                        and not os.path.realpath(w).startswith(os.path.realpath(tmp)):
                    libdirs.append(os.path.dirname(w))
                    libs.append(":" + os.path.basename(w))
        try:
            g = subprocess.run([cc, "--sysroot=" + sdk, "-print-libgcc-file-name"], capture_output=True, text=True,
                               timeout=60)
            lg = g.stdout.strip().splitlines()
            if lg and os.path.isfile(lg[-1]):
                out.append(f"libgcc {lg[-1]} sha256 {sha(lg[-1])}")
        except OSError:
            pass
    for p in sorted(set(progs)):
        out.append(f"program {p} -> {os.path.realpath(p)} sha256 {sha(os.path.realpath(p))}")
    sdkreal = os.path.realpath(sdk)
    found = set()
    for d in libdirs:
        if not os.path.isdir(d) or os.path.realpath(d).startswith(sdkreal):
            continue  # the SDK is identified as a whole
        for l in libs:
            name = l[1:] if l.startswith(":") else f"lib{l}.a"
            p = os.path.join(d, name)
            if os.path.isfile(p) and p not in found:
                found.add(p)
                out.append(f"toolchain-lib {p} sha256 {sha(p)}")
    if not progs:
        out.append("program (driver did not answer -###; identified by its file and version only)")
    return out


def main():
    if len(sys.argv) < 8:
        raise SystemExit(__doc__)
    root, target, cc, ar, sdk, ossl, jent = sys.argv[1:8]
    options = " ".join(sys.argv[8:]).replace(root + "/", "")
    out = [f"target {target}", f"cc {tool(cc)}", f"ar {tool(ar)}"] + behind(cc, sdk)
    digest, n = tree(sdk)
    out.append(f"sdk {sdk} include+lib {n} files sha256 {digest}")
    out.append(f"openssl-source {os.path.basename(ossl)} sha256 {sha(ossl)}")
    out.append(f"jitter-source {os.path.basename(jent)} sha256 {sha(jent)}")
    recipe = ["scripts/build-openssl.sh", "scripts/build-jitterentropy.sh", "scripts/env.sh",
              "scripts/dep-inputs.py", "config/openssl/50-aros-syncthing.conf",
              "config/jitterentropy/jitterentropy-base-user.h"]
    pdir = os.path.join(root, "patches", "openssl-4.0.1")
    recipe += [os.path.join("patches", "openssl-4.0.1", p) for p in sorted(os.listdir(pdir)) if p.endswith(".patch")]
    for r in recipe:
        out.append(f"recipe {r} sha256 {sha(os.path.join(root, r))}")
    out.append(f"options {options}")
    print("\n".join(out))


main()
