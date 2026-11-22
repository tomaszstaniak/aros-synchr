# AROS JitterEntropy platform adapter

Used with the unmodified jitterentropy-library 3.7.0 sources, whose archive
SHA-256 is checked by `scripts/build-jitterentropy.sh`. The platform layer
uses the x86 timestamp counter, cleared allocation/free, one CPU and the
library's default cache sizing. It does not override initialization or
continuous health tests, enable the internal timer thread, or add an RNG
fallback. The library must be compiled with `-O0`.

The adapter is retained from the existing AROS JitterEntropy port under the
MIT license in `LICENSE.port`. Upstream sources and their BSD/GPL license
files are extracted only into ignored per-target dependency output.

## Provenance

Copied unchanged from `arospkg` (origin
`https://github.com/tomaszstaniak/arospkg.git`), inspected commit
`3e6bd5529f876e7fde19b024021bc381571bd70d`:

- `src/tls/jent-aros/jitterentropy-base-user.h`, SHA-256
  `35a303db76f86984aee2599cecb4d8b64488834d8962aae8d0701b59a788b2e8`.
- `LICENSE` → `LICENSE.port`, SHA-256
  `abbda66a38af05b9bbf5e4ab6ed18fe85329be3dbd18c50b2deab13712db30fc`.

This is a pinned build dependency copy; the originating port remains owned
by `arospkg`. It is not a new upstream release.
