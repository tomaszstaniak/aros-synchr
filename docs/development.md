---
type: guide
updated: 2026-11-09
---

# Development

## Current scope

`stpoc` is a proof of concept. Its `twoway` mode (below) is the first
version of two-way synchronisation; the rest of this section describes
the receive-only modes `run` and `sync`. It connects
to one Syncthing device over TLS 1.3, authenticates it by Device ID, takes
part in the BEP exchange for one folder (Hello, ClusterConfig, an Index of
its own) and reports the files in the peer's Index and IndexUpdate
messages. The peer's index counts as complete once the sequence that the
peer announced for itself in its ClusterConfig has arrived through a chain
of batches, each IndexUpdate's `prev_sequence` following on from the one
before (peers must send these fields, as Syncthing v2 does): Syncthing
sends a large index as an Index followed by IndexUpdates, and an empty
folder as no Index at all. A session that ends before that is
`INDEX_INCOMPLETE`, never `PASS`. In `sync` mode it also downloads the files into a
destination directory (block by block, each checked against its SHA-256,
written under a temporary name and renamed when complete) and announces
each file once written. A state file next to the identity remembers what
was written; at start every recorded file is hashed again, so unchanged
files are not downloaded after a restart, and files changed locally are
kept and reported as conflicts instead of being overwritten. Just before a
downloaded file takes its final name, the previous version is moved aside
and hashed again; if it was changed here during the download (or a file
appeared where there was none), it is put back and reported as a
conflict. A previous version left moved aside by an interruption between
the two renames is put back at the next start. When both the file and such
a moved-aside copy (`.stpoc-<name>.old`) are here, the copy is dropped only
if it is exactly the recorded version; otherwise it is kept, never deleted
by a later replacement, and the name is reported as a conflict until the
copy is moved or deleted. This protects changes made before the check of
the moved-aside file; a program that keeps the file open and writes to it
during or after that check, and changes between the last check of a path
and the rename, are not covered.

Results (the log's last line): `PASS` only when the peer's whole index
arrived and every entry was received or deliberately skipped (deletions,
entries the peer marks invalid); `PARTIAL` when entries could not be
received here (names outside the accepted character set, too long or
colliding, invalid block lists, symbolic links); `LOCAL_CONFLICTS`,
`FILES_FAILED`, `STATE_NOT_SAVED` (records still unwritten after the last
retry; failed writes are retried after 5 s, doubling to 60 s, whatever the
number of pending records), `INDEX_INCOMPLETE`, `INDEX_SEQUENCE_ERROR` (an IndexUpdate that
does not follow on from the batch before), `INDEX_RESET_UNSUPPORTED` (the
peer started its index again within the session) and transport errors. It applies no deletions,
does not resume partial downloads and does not handle symbolic links.

### Two-way mode (`twoway`, first version)

`twoway` synchronises one folder with one peer in both directions
(milestone and rules: the project's two-way backlog entry). The folder is
offered as Send & Receive. Local files are found by reading every
directory once (on AROS with `Examine`/`ExNext`, so no name is looked up
on its own). Where the volume dates every write (SFS, FFS/OFS, host
filesystems) a file is hashed when size or date differ from the record,
or when the date lies within 2 s of the hashing; on FAT, and on any volume
type not known to date writes, every file is hashed at each scan, because
AROS fat-handler leaves the date of a written file unchanged. The log's
first `scan:` line names the policy. Unchanged content is never a new
version and does not rewrite the state. A new or
changed file gets a new version (our counter becomes max(old + 1, now) as
in Syncthing v2.1.7) and the next sequence number; the record is written
to the state file before the IndexUpdate that announces it, so no
sequence number is announced twice for different content. Our index ID
and sequence are kept in the state file (format 2); at connection the
peer's ClusterConfig says how far it has our index, and only what follows
is sent. A peer that claims more of our index than is recorded makes us
take a new index ID (`INDEX_ID_RENEWED`; the next session sends a full
index). The peer's block requests are answered only for blocks of what we
announced, after reading and hashing the block again; a mismatch (an edit
that kept size and date) is answered `NO_SUCH_FILE` and the file is
hashed in full at the next scan. The peer's entries go through the
decision table (`src/decide.c`): newer files and directories are
downloaded, an equal content under a newer version is recorded.

Deletions go both ways. A file or directory missing here is published as
deleted (a newer version, as Syncthing's `SetDeleted` makes it). A
deletion from the peer is applied when what is here is still our
recorded version; the file is moved aside, checked, then removed. A
directory is removed only when empty; otherwise it stays and is published
again. Edits on both sides follow Syncthing's winner rule: when the peer's
version wins, ours is kept as `name.sync-conflict-YYYYMMDD-HHMMSS-<id>.ext`
and published; when ours wins, the peer makes its own copy, which comes
here like any new file. An edit against a deletion goes the same way: a
winning deletion leaves our edit as a conflict copy, a winning edit brings
the file back. A conflict copy that loses again is not copied a second
time (Syncthing's rule). A changed file between file and directory is
still held and reported. At the start, temporary files left by an
interrupted download are deleted and a file moved aside by an
interrupted replacement or deletion is put back.
A missing file counts as deleted only under a directory read completely;
the folder must hold `.stfolder` (created on the first run, as Syncthing
does), otherwise it is `FOLDER_UNAVAILABLE` and nothing is scanned or
written. A first run, or a damaged state (kept as `<state>.rejected`),
gives local files provisional versions that are settled against the
peer's complete index first: equal content takes the peer's version. A
receive-only state (format 1) is migrated: files still matching keep the
peer's version, changed ones become our change on top of it, missing ones
are dropped and downloaded again. Results: `PASS` when both indexes agree
on every entry and the last scan verified every file (conflict copies
made are counted in the summary, not a failure), `UNCHECKED` when it
could not (an unreadable file or directory, a file that changed while it
was read, a refused block request not yet followed by a scan), `PARTIAL`
with held or unsupported entries, `NOT_IN_SYNC`, `FILES_FAILED`,
`STATE_NOT_SAVED`, `FOLDER_UNAVAILABLE`.

Limits of the destination: names must be ASCII without `:` or `\`, a
path component at most 80 bytes; the temporary name adds 12, so the file
system must take names of about 92 characters (SFS and FAT32 long names
do, the classic FFS/OFS 30-character names do not; such files fail
without touching what is there). Replacements are renames without
`fsync()`: an operation that fails is handled, but data the file system
has not written out at a power loss is not protected. Folders of type
"receive encrypted" on the peer are not supported.

Build profiles: `x86_64-aros-v1` (mainline), `x86_64-aros-v11` and
`aarch64-aros-v1` (native Raspberry Pi). Each needs its own toolchain and
SDK; check the SDK against the runtime used for testing.

## Layout

| path | contents |
|---|---|
| `src/` | `devid` (Device ID text form), `net` (TCP with an overall deadline), `tls` (OpenSSL: identity, socket BIO, TLS 1.3, Device ID pinning), `pb` (protobuf wire format), `bep` (BEP messages), `lz4blk` (LZ4 block decoder), `frame` (BEP framing), `store` (name checks and replacement by rename under the destination), `fetch` (block requests, verification, announcing; receive-only), `state` (the receive-only state file, format 1), `stfile` (the checksummed container both state formats use), `idxprog` (completeness of the peer's index), `vv` (version vectors), `finfo` (FileInfo values, conflict rules, hashing), `decide` (the two-way decision table), `db` (the two-way state, format 2), `scan` (reading the folder), `tw` (the two-way engine), `session` (one BEP session), `main` (the `stpoc` program) |
| `experiments/tls-probe/` | the earlier TLS-only probe and its AROS Shell script |
| `experiments/zune-preview/` | `stgui`, a Zune preview of a front end that follows `stpoc --status` |
| `tests/` | host unit tests; `tests/aros/` AROS Shell scripts for runs on AROS |
| `scripts/` | `env.sh` (settings and build profiles), `dep-inputs.py` (identity of a dependency build), `build-openssl.sh`, `build-jitterentropy.sh`, `build.sh`, `test-host.sh`, `twoway-host-test.sh`, `compat-fixtures.sh`, `st-peer.sh`, `peer-change-after-connect.sh`, `aros-sync-test.sh`, `aros-state-test.sh`, `test-state-host.sh`, `make-demo-folder.sh`, `make-demo-stage.sh` |
| `config/openssl/` | the OpenSSL Configure target used for AROS |
| `patches/openssl-4.0.1/` | changes applied to the OpenSSL release; each header explains why |

Generated and local material (`build/<target>/`, `deps/<target>/`, `var/`,
`local.env`) is ignored by Git. Build and dependency outputs are separate
per architecture and ABI.

## Configuration

Copy `local.env.example` to `local.env` and set the toolchain, SDK,
OpenSSL tarball, jitterentropy package, host OpenSSL, host liblz4 (tests
only) and Syncthing binary.
Environment variables override `local.env`. `ST_TARGET` selects the target:
`x86_64-aros-v1`, `x86_64-aros-v11` or `aarch64-aros-v1`. Set `AROS_TC` and
`AROS_SDK` separately for each target; a target name alone does not choose
the toolchain. For x86_64, `AROS_TC` holds `x86_64-aros-gcc`; for the
Raspberry Pi it is the toolchain root with `bin/aarch64-aros-gcc` (a Clang
wrapper that also takes the SDK from `AROS_SDK`). Keep each SDK paired with
the runtime used for acceptance.

## Dependencies

- **OpenSSL 4.0.1** (Apache-2.0), built statically per target by
  `scripts/build-openssl.sh` from the release tarball (SHA-256 checked)
  with the patches in this repository. The SDK's own OpenSSL is not used.
- **jitterentropy 3.7.0** (BSD-3-Clause or GPL-2.0) as the only RNG seed
  source on AROS, through OpenSSL's built-in JITTER source
  (`enable-jitter`, `--with-rand-seed=none`), supplied as an AROS developer
  package (`libjitterentropy.a` and headers) on v11. For the v1 targets
  (x86_64 and Raspberry Pi), set `JENT_TARBALL` to the upstream 3.7.0
  release archive; the build verifies its SHA-256 and compiles it with the
  selected SDK, `-O0` and no internal timer thread. Its timer is the time
  stamp counter on x86_64 and the virtual counter (`cntvct_el0`) on aarch64. The AROS adapter in `config/jitterentropy/` retains its MIT license.
- `bsdsocket.library` at run time. Tested with AROSTCP only.

## Build and test

```sh
# v1 or Pi: export matching AROS_TC, AROS_SDK and JENT_TARBALL first
# export ST_TARGET=x86_64-aros-v1   (or aarch64-aros-v1)
scripts/build-openssl.sh            # per target; rebuilds when an input changed
scripts/build.sh aros               # build/<target>/stpoc
scripts/build.sh host               # build/host/stpoc
scripts/build.sh aros tlsprobe      # the TLS probe
scripts/test-host.sh                # unit tests with ASan and UBSan
scripts/twoway-host-test.sh         # two-way flow against an unmodified Syncthing (own instance)
scripts/stagec-host-test.sh         # deletions, conflicts, interruptions against the same
SYNCTHING_SRC=... scripts/compat-fixtures.sh  # regenerate tests/compat/syncthing-cases.txt
```

`tests/compat/syncthing-cases.txt` holds cases produced by Syncthing's own
code (`tests/compat/oracle.go`: Vector Compare/Update/Merge, WinsConflict,
InConflictWith, ShortID text, BlockSize, BlocksHash, FileInfo encoding,
conflict names); `test_vv` checks our implementation against every line.
The file records the Syncthing version it came from.

`stpoc` usage (the log's last line is `RESULT PASS` or `RESULT FAIL <reason>`):

```text
stpoc LOGFILE identity-create DIR
stpoc LOGFILE identity-show DIR
stpoc LOGFILE run DIR IPV4 PORT PEER-DEVICE-ID FOLDER-ID [SECONDS [UPDATES [NAME]]]
stpoc LOGFILE sync DIR IPV4 PORT PEER-DEVICE-ID FOLDER-ID DEST [SECONDS [UPDATES [ABORT-AFTER-BLOCKS [STATE-FAULT [COMMIT-HOOK-DIR]]]]]
stpoc LOGFILE twoway DIR IPV4 PORT PEER-DEVICE-ID FOLDER-ID FOLDER [SECONDS [RESCAN-SECONDS]]
```

`twoway` ends when both indexes agree (or after SECONDS, default 60);
`RESCAN-SECONDS` > 0 reads the folder again that often during the session.

`UPDATES` counts IndexUpdates received after the complete index whose
changes lie beyond what the peer's ClusterConfig announced.

The state of `DEST` is kept in `DIR` as `state-<hash of the folder ID>.txt`
(a checksummed text file; a damaged one is ignored and the files are then
checked against the peer's index). Deleting it is safe: matching files are
adopted without downloading. `COMMIT-HOOK-DIR` (test hook, `-` for none):
before each file is committed, `stpoc` creates `at-<name>` there and waits
up to 120 s for `go-<name>`, so a test can change the destination at that
point. `STATE-FAULT 1` is a test hook that makes every
state write fail.

`--status FILE` (before LOGFILE) appends the same short user-facing lines
as `--brief` to FILE, closing it after each line, so that a front end can
follow a running session. `experiments/zune-preview/stgui` is such a front
end, a preview only (one window for one folder, standard Zune classes, no
settings): `stgui STPOC IDDIR IP PORT PEER FOLDER DEST [TIMEOUT=s]
[RECEIVEONLY]` (default 3600 s; sessions run `twoway`, `RECEIVEONLY` runs
`sync`), built with `scripts/build.sh aros stgui`. A helper
process runs stpoc and reports its exit, so a session that ends without a
`Result:` line (crash, failed status write) is shown as such, with the
return code SystemTags reported (on v1 a missing command gave 0, so the
code alone does not tell success);
status and log files are `T:stgui-<task>.txt/.log` per instance;
arguments with spaces are quoted, a double quote or `*` is refused. It
starts the command-line client per session; a product front end needs a
long-running engine with its own state interface instead (see the
backlog).

`DEST` must exist. `ABORT-AFTER-BLOCKS` is a test hook that stops the
transfer like a crash after that many blocks. `--brief` (before LOGFILE)
limits the console to short user-facing lines: connection, folder, each
file with its size and verification, and a summary; the full log still
goes to LOGFILE.

A local Syncthing peer with its own configuration directory, which does not
touch any other Syncthing installation:

```sh
scripts/st-peer.sh init && scripts/st-peer.sh start
mkdir -p var/id && build/host/stpoc var/c.log identity-create var/id  # prints our Device ID
scripts/st-peer.sh share <our-device-id> test         # folder aros-poc-1 in var/st-folder
scripts/peer-change-after-connect.sh <our-device-id> & # edits a file once we connect
build/host/stpoc var/r.log run var/id 127.0.0.1 22100 "$(scripts/st-peer.sh id)" aros-poc-1 90 1
scripts/st-peer.sh reset                              # stop and remove the peer and folder
```

On AROS, unpack `stpoc` and `tests/aros/bep.scr` into `RAM:stp` and run
`Execute RAM:stp/bep.scr <tag> <peer-id> <wrong-id>` while the host runs
`peer-change-after-connect.sh`; results go to `RESULTS:bep-<tag>-*`. A QEMU
guest with user networking reaches the host's peer at `10.0.2.2:22100`.

The file transfer test on a test-machine pool is driven from the host,
which also enforces a real-time limit (guest clocks under emulation are not
reliable):

```sh
AROS_VM_OWNER=<session> AROS_DEVICE_ID=<aros-device-id> \
  scripts/aros-sync-test.sh <machine> <tag> <stage-dir> <evidence-dir> [limit-seconds]
```

`<stage-dir>` holds `stpoc` and `tests/aros/sync.scr`.

Persistent-state scenarios (first run, restart without requests, a change on
the peer, a file deleted or changed locally, a failing state write, a
damaged or deleted state file): `scripts/test-state-host.sh <device-id>
<identity-dir>` on the host, and on a pool machine
`AROS_VM_OWNER=<session> AROS_DEVICE_ID=<id> scripts/aros-state-test.sh <machine> <tag> <evidence-dir>`,
which runs `tests/aros/state.scr` with a real-time limit.

Acceptance on one AROS target in a single boot (RNG refusal and forced
failure, a new identity, TLS with the right and a wrong Device ID, first
download, restart, a change on the peer, a local conflict, recovery of an
interrupted replacement, a two-batch index and an empty folder, written to
the target's `SYS:`): `scripts/aros-accept.sh setup` prepares an isolated
peer's folders and the stage with `tests/aros/accept.scr`; on the guest
copy the stage to `RAM:stp` and `Execute RAM:stp/accept.scr`, while
`scripts/aros-accept.sh watch` changes the peer at the guest's marker.
Give every guest that runs at the same time its own test identity
(`ST_ACCEPT_ID`): Syncthing keeps one connection per device.

A demonstration with neutral content: `scripts/make-demo-folder.sh` creates
`var/st-demo`; share it with
`ST_PEER_FOLDER_ID=aros-demo ST_PEER_FOLDER_LABEL="AROS demo" ST_PEER_FOLDER_DIR=$PWD/var/st-demo scripts/st-peer.sh share <aros-device-id> <name>`;
`scripts/make-demo-stage.sh <tag> <stage-dir>` builds the stage (with the
Limpet terminal when `LIMPET_ZIP` is set); on AROS, `Execute RAM:stp/run-demo`
receives the folder into `SYS:Syncthing-demo`.

## Protocol reference

Use the [BEP specification](https://docs.syncthing.net/specs/bep-v1.html).
Syncthing accepts only TLS 1.3 for device connections, and since v2.0.0 it
creates Ed25519 device certificates, so the TLS library must verify
Ed25519 signatures. A compatible client must authenticate peers, validate
received data and paths, and retain enough synchronization state to
reconnect correctly; beyond peer authentication these are not implemented.

## Isolated acceptance peer

`ST_PEER_HOME` selects an absolute private peer home. Set it together with
`ST_PEER_FOLDER_DIR`, `ST_PEER_FOLDER_ID`, `ST_PEER_PORT` and
`ST_PEER_GUI_PORT` when testing without reusing existing demo state. The
peer still binds to loopback, disables discovery/relays and shares only
the explicitly configured folder. Stop that peer after testing.

Both AROS profiles deliberately select OpenSSL's JITTER seed source and
disable all fallback seed sources. Availability of system `getentropy()`
alone does not establish equivalent fail-closed entropy behavior. Runtime
acceptance must test missing source, forced JitterEntropy initialization
failure, successful TLS 1.3 and refusal of the wrong peer Device ID.

`scripts/build-openssl.sh` records the identity of the dependency build's
inputs in `deps/<target>/openssl/INPUTS.txt` (`scripts/dep-inputs.py`):
target; the compiler driver, its version and the programs it runs to
compile and link (from `-###`, asked with the same `--sysroot`), libgcc or
the builtins archive and other toolchain libraries it links from outside
the SDK; the SHA-256 of every file under the SDK's `include/` and `lib/`,
following symbolic links; the OpenSSL and jitterentropy sources, the
patches, the build recipe and the options. An installed tree is reused
only when that record equals the current inputs; otherwise it is rebuilt,
and the previous tree is replaced only by a complete new one.
`scripts/build.sh aros` refuses to link against a tree whose record
differs. Not identified: programs started by those programs (the linker
that `collect-aros` runs, a plugin) and libraries they load.
OpenSSL's built-in compiler string is
replaced by a neutral one, and a library containing a workstation path is
not installed. The JitterEntropy builder always uses a fresh temporary
source/object tree and replaces its archive only on success.
