"""The dependency manifest must change with every input of an OpenSSL build,
also when an SDK or compiler is replaced at the same path."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DepInputs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="st-dep-inputs-")
        t = Path(self.tmp.name)
        self.root = t / "repo"
        for d in ["scripts", "config/openssl", "config/jitterentropy", "patches/openssl-4.0.1"]:
            (self.root / d).mkdir(parents=True)
        for f in ["scripts/build-openssl.sh", "scripts/build-jitterentropy.sh", "scripts/env.sh",
                  "scripts/dep-inputs.py", "config/openssl/50-aros-syncthing.conf",
                  "config/jitterentropy/jitterentropy-base-user.h"]:
            shutil.copy2(ROOT / f, self.root / f)
        (self.root / "patches/openssl-4.0.1/0001-x.patch").write_text("patch 1")
        self.tc = t / "tc"
        self.tc.mkdir()
        for tool in ["x86_64-aros-gcc", "x86_64-aros-ar"]:
            p = self.tc / tool
            p.write_text("#!/bin/sh\necho 'fixture-cc 1.0'\n")
            p.chmod(0o755)
        self.sdk = t / "sdk"
        (self.sdk / "include/aros").mkdir(parents=True)
        (self.sdk / "lib").mkdir()
        (self.sdk / "include/aros/system.h").write_text("v1")
        (self.sdk / "lib/libc.a").write_text("lib v1")
        self.ossl = t / "openssl.tar.gz"
        self.ossl.write_text("openssl")
        self.jent = t / "jent.tar.gz"
        self.jent.write_text("jent")

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self, *options):
        r = subprocess.run(["python3", "-I", str(self.root / "scripts/dep-inputs.py"), str(self.root),
                            "x86_64-aros-v1", str(self.tc / "x86_64-aros-gcc"), str(self.tc / "x86_64-aros-ar"),
                            str(self.sdk), str(self.ossl), str(self.jent), *options],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_every_input_changes_the_manifest(self):
        base = self.manifest("no-shared")
        self.assertEqual(base, self.manifest("no-shared"), "not deterministic")
        changes = [
            lambda: (self.sdk / "include/aros/system.h").write_text("v2"),          # SDK header, same path
            lambda: (self.sdk / "lib/libc.a").write_text("lib v2"),                 # SDK library
            lambda: (self.tc / "x86_64-aros-gcc").write_text("#!/bin/sh\necho 'fixture-cc 1.0'\n#x\n"),
            lambda: (self.root / "patches/openssl-4.0.1/0001-x.patch").write_text("patch 2"),
            lambda: (self.root / "config/openssl/50-aros-syncthing.conf").write_text("changed"),
            lambda: self.ossl.write_text("other openssl"),
            lambda: self.jent.write_text("other jent"),
        ]
        prev = base
        for i, change in enumerate(changes):
            change()
            now = self.manifest("no-shared")
            self.assertNotEqual(prev, now, f"change {i} not reflected")
            prev = now
        self.assertNotEqual(prev, self.manifest("no-shared", "enable-x"), "options not reflected")

    def test_symlinked_sdk_content_is_identified(self):
        """Review 2026-11-09: a header reached through a symbolic link, and a
        linked directory, are identified by their content."""
        t = Path(self.tmp.name)
        real = t / "real-headers"
        (real / "sub").mkdir(parents=True)
        (real / "linked.h").write_text("v1")
        (real / "sub" / "deep.h").write_text("deep v1")
        (self.sdk / "include" / "linked.h").symlink_to(real / "linked.h")
        (self.sdk / "include" / "linkeddir").symlink_to(real / "sub")
        (self.sdk / "include" / "loop").symlink_to(self.sdk / "include")  # must not hang
        base = self.manifest()
        (real / "linked.h").write_text("v2")
        after_file = self.manifest()
        self.assertNotEqual(base, after_file, "content behind a file link not identified")
        (real / "sub" / "deep.h").write_text("deep v2")
        self.assertNotEqual(after_file, self.manifest(), "content in a linked directory not identified")

    def test_programs_behind_the_driver_are_identified(self):
        """Review 2026-11-09: the programs the driver runs (cc1, as, ld) are
        identified, not only the driver and its --version text."""
        t = Path(self.tmp.name)
        cc1 = t / "libexec" / "cc1"
        cc1.parent.mkdir()
        cc1.write_text("#!/bin/sh\n# cc1 v1\n")
        cc1.chmod(0o755)
        lib = t / "tclib"
        lib.mkdir()
        (lib / "libgcc.a").write_text("libgcc v1")
        drv = self.tc / "x86_64-aros-gcc"
        drv.write_text("#!/bin/sh\n"
                       "case \"$*\" in *-print-libgcc-file-name*) echo %s; exit 0;; esac\n"
                       "case \"$*\" in *-###*) echo \" \\\"%s\\\" -quiet x.c\" >&2; "
                       "echo \" \\\"%s\\\" -o x\" >&2; exit 0;; esac\n"
                       "echo 'fixture-cc 1.0'\n" % (lib / "libgcc.a", cc1, cc1))
        base = self.manifest()
        cc1.write_text("#!/bin/sh\n# cc1 v2\n")
        after = self.manifest()
        self.assertNotEqual(base, after, "program behind the driver not identified")
        (lib / "libgcc.a").write_text("libgcc v2")
        self.assertNotEqual(after, self.manifest(), "libgcc not identified")

    def test_missing_sdk_parts_refused(self):
        shutil.rmtree(self.sdk / "lib")
        r = subprocess.run(["python3", "-I", str(self.root / "scripts/dep-inputs.py"), str(self.root),
                            "x86_64-aros-v1", str(self.tc / "x86_64-aros-gcc"), str(self.tc / "x86_64-aros-ar"),
                            str(self.sdk), str(self.ossl), str(self.jent)], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
