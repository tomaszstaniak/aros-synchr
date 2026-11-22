"""Exercise build publication with disposable tools, SDK and source archive."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class FreshBuild(unittest.TestCase):
    def test_stale_inputs_excluded_and_failed_rebuild_preserves_library(self):
        with tempfile.TemporaryDirectory(prefix="st-jent-build-") as tmp:
            root = Path(tmp)
            scripts = root / "scripts"
            scripts.mkdir()
            for name in ["env.sh", "build-jitterentropy.sh"]:
                shutil.copy2(ROOT / "scripts" / name, scripts / name)
            config = root / "config/jitterentropy"
            config.mkdir(parents=True)
            for name in ["jitterentropy-base-user.h", "LICENSE.port"]:
                (config / name).write_text("fixture")
            src = root / "fixture/jitterentropy-library-3.7.0"
            (src / "src").mkdir(parents=True)
            for name in ["jitterentropy-base-user.h", "jitterentropy.h", "LICENSE", "LICENSE.bsd", "LICENSE.gplv2", "src/fresh.c"]:
                (src / name).write_text("fixture")
            archive = root / "fixture.tar.gz"
            with tarfile.open(archive, "w:gz") as tf:
                tf.add(src, arcname=src.name)
            # Substitute only the pinned fixture checksum in this disposable copy.
            # The production hash rejection is exercised by DependencyIntegrity.
            builder = scripts / "build-jitterentropy.sh"
            builder.write_text(builder.read_text().replace("f5eaccc9d2977c83308651be9379f09f34348398f419e8f8b5bbd95928c777ed", hashlib.sha256(archive.read_bytes()).hexdigest()))
            tc = root / "tc"
            tc.mkdir()
            cc = tc / "x86_64-aros-gcc"
            cc.write_text("#!/usr/bin/env python3\nimport os,pathlib,sys\nif os.environ.get('FAIL_COMPILE'): sys.exit(9)\npathlib.Path(sys.argv[sys.argv.index('-o')+1]).write_text('object')\n")
            ar = tc / "x86_64-aros-ar"
            ar.write_text("#!/usr/bin/env python3\nimport pathlib,sys\np=pathlib.Path(sys.argv[2]); old=p.read_text() if p.exists() else ''\np.write_text(old+''.join(pathlib.Path(x).name+'\\n' for x in sys.argv[3:]))\n")
            cc.chmod(0o755); ar.chmod(0o755)
            sdk = root / "sdk/include/aros"
            sdk.mkdir(parents=True)
            (sdk / "system.h").write_text("fixture")
            (root / "sdk/lib").mkdir()
            deps = root / "deps/x86_64-aros-v1"
            oldsrc = deps / "src/jitterentropy-library-3.7.0/src"
            oldsrc.mkdir(parents=True)
            (oldsrc / "stale.c").write_text("old source")
            oldobj = root / "build/x86_64-aros-v1/jitterentropy-obj"
            oldobj.mkdir(parents=True)
            (oldobj / "stale.o").write_text("old object")
            library = deps / "jitterentropy/JitterEntropy-3.7.0-local/lib/libjitterentropy.a"
            library.parent.mkdir(parents=True)
            library.write_text("stale-member.o\n")
            env = dict(os.environ, ST_TARGET="x86_64-aros-v1", AROS_TC=str(tc), AROS_SDK=str(root / "sdk"), JENT_TARBALL=str(archive))
            result = subprocess.run(["bash", str(builder)], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(library.read_text(), "fresh.o\n")
            before = library.read_bytes()
            result = subprocess.run(["bash", str(builder)], env=dict(env, FAIL_COMPILE="1"), capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(library.read_bytes(), before)
            self.assertEqual(list(deps.glob(".jitter-build.*")), [])

if __name__ == "__main__":
    unittest.main()
