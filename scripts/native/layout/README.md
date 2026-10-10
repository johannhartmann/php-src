# Runtime function layout

The Linux release profiles set `PROFILE_FUNCTION_ORDER` to
`linux-amd64-apps.order`. `scripts/native/build.sh` then compiles with
`-ffunction-sections` and links with `ld --section-ordering-file` (binutils
2.43 or newer; an older linker keeps the default layout). The hot runtime
functions of warm WordPress, Symfony and Laravel requests come first in
`.text`, each next to its most frequent caller.

This removes instruction-fetch stalls, and it keeps the hot runtime code in
place when unrelated objects change size. Without it, an edit to the TPDE
compiler moves every Zend runtime function and shifts A/B cycle measurements
by about one million cycles per request.

## Regenerating the order

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
