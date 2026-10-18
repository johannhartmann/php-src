# Runtime function layout and PGO

The Linux release profiles (`linux-amd64-native-release-nts`,
`-product-release-nts`, `-app-release-nts`) optimize the C and C++ runtime
with the two artifacts in this directory. [`TPDE.md`](../../../TPDE.md)
section 6 gives the overview; this file has the recipes.

## Function order

The release profiles set `PROFILE_FUNCTION_ORDER` to
`linux-amd64-apps.order`. `scripts/native/build.sh` then compiles with
`-ffunction-sections` and links with `ld --section-ordering-file` (binutils
2.43 or newer; an older linker keeps the default layout). The hot runtime
functions of warm WordPress, Symfony and Laravel requests come first in
`.text`, each next to its most frequent caller.

This removes instruction-fetch stalls, and it keeps the hot runtime code in
place when unrelated objects change size. Without it, an edit to the TPDE
compiler moves every Zend runtime function and shifts A/B cycle
measurements.

### Regenerating the order

Profile warm requests of each application with callgrind, using a binary
built from one of these profiles, then run:

```sh
scripts/native/layout/function-order.py \
    --binary <build>/sapi/fpm/php-fpm \
    --output scripts/native/layout/linux-amd64-apps.order \
    <callgrind output of a warm request>@<weight>...
```

Weight each profile by the inverse of its request's instruction count,
divided among the application's pages, so that every application counts
alike (WordPress `/`, `/?s=hello`, `/sample-page/`; Symfony `/en/blog/` and
a blog post; Laravel `/`).

Functions missing from a later build are ignored by the linker. A changed
order file rebuilds the profile.

## Profile-guided optimization

The same release profiles set `PROFILE_PGO_PROFILE` to
`linux-amd64-apps.profdata.xz`, an xz-compressed clang profile
(`llvm-profdata`) of warm WordPress, Symfony and Laravel requests.
`scripts/native/build.sh` unpacks it into the profile's work directory and
compiles the C and C++ runtime with `-fprofile-instr-use`: hot paths are laid
out straight, inlined by measured frequency, and never-run functions move to
`.text.unlikely`. Functions changed since the profile was taken lose only
their own profile (the out-of-date warnings are silenced). Without `xz` or
with a compiler that cannot read the profile, the build continues without
it; `NATIVE_PGO=off` disables it.

### Regenerating the profile

The clang that reads the profile must match the `llvm-profdata` that merges
it (same LLVM major version).

1. Build an instrumented binary from one of these profiles without the old
   profile:

   ```sh
   NATIVE_PGO=off EXTRA_CFLAGS=-fprofile-instr-generate \
   EXTRA_CXXFLAGS=-fprofile-instr-generate \
       scripts/native/build.sh --profile linux-amd64-native-app-release-nts
   ```

2. Run each application's FPM pool (trained branch layout, the FPM default)
   with `LLVM_PROFILE_FILE=<dir>/%4m.profraw` and serve a few hundred warm
   requests of the pages listed above, so that the request runtime outweighs
   the one-time native compilation. Stop the workers with `SIGQUIT` before
   the master: a worker writes its profile when it leaves its request loop,
   and the master's own shutdown kills them before that.

3. Merge and compress:

   ```sh
   llvm-profdata merge -o apps.profdata <dir>/*.profraw
   xz -9 -c apps.profdata > scripts/native/layout/linux-amd64-apps.profdata.xz
   ```

Keep the full profile (not `--sparse`): it records which functions never ran,
which places them cold.
