"""A test peer override must isolate configuration, identity and folder state."""
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]

class PeerIsolation(unittest.TestCase):
    def test_explicit_home(self):
        with tempfile.TemporaryDirectory(prefix="st-peer-isolation-") as tmp:
            tmp = Path(tmp)
            fake = tmp / "syncthing"
            fake.write_text("#!/usr/bin/env python3\nimport pathlib,sys\np=pathlib.Path(next(x.split('=',1)[1] for x in sys.argv if x.startswith('--home=')))\np.mkdir(parents=True,exist_ok=True)\n(p/'config.xml').write_text('<configuration><folder id=\"default\"/><options/><gui><address/><apikey>test-only</apikey></gui></configuration>')\n")
            fake.chmod(0o755)
            home = tmp / "private-peer"
            scripts = tmp / "checkout/scripts"
            scripts.mkdir(parents=True)
            for name in ["env.sh", "st-peer.sh"]:
                shutil.copy2(ROOT / "scripts" / name, scripts / name)
            env = dict(os.environ, SYNCTHING_BIN=str(fake), ST_PEER_HOME=str(home), ST_PEER_PORT="22991", ST_PEER_GUI_PORT="8399")
            result = subprocess.run(["bash", str(scripts / "st-peer.sh"), "init"], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((home / "config.xml").exists(), result.stdout)
            self.assertFalse((tmp / "checkout/var/st-peer").exists())
            config = ET.parse(home / "config.xml").getroot()
            self.assertEqual(config.findtext("options/listenAddress"), "tcp://127.0.0.1:22991")
            self.assertEqual(config.findtext("options/globalAnnounceEnabled"), "false")
            self.assertEqual(config.findall("folder"), [])

class DependencyIntegrity(unittest.TestCase):
    def test_rejects_modified_jitter_source_before_using_tools(self):
        with tempfile.TemporaryDirectory(prefix="st-jent-integrity-") as tmp:
            archive = Path(tmp) / "modified.tar.gz"
            archive.write_bytes(b"not the pinned release")
            env = dict(os.environ, ST_TARGET="x86_64-aros-v1", JENT_TARBALL=str(archive), AROS_TC="/nonexistent/compiler", AROS_SDK="/nonexistent/sdk")
            result = subprocess.run(["bash", str(ROOT / "scripts/build-jitterentropy.sh")], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("tarball hash mismatch", result.stderr)

if __name__ == "__main__":
    unittest.main()
