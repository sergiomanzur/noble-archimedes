# Content-watch exact comparison: SSE2 experiment notes (production retired)

## Outcome (2026-10-22)

**Rejected for production adoption.** The authorized six-run contended gameplay
comparison produced no repeatable frame-interval gain:

- candidate minus baseline p50 pairs: **+1.345 ms, -0.699 ms, +0.167 ms**
- arm medians: baseline **17.580 ms**, candidate **18.3685 ms**
- pairs are mixed; arm non-overlap is false; both arm spreads exceed 1.5 ms
- host was contended, so this is **not a proven regression**
- uncertainty: all six strict preflights failed; candidate 1 and 3 also had
  DSP stall-overrun validation events, and candidate 3 emitted one
  unexpected `[NV2A] SET_SURFACE_PITCH` line. These are reported, not
  excluded; no clean-performance verdict is claimed.

Artifact directory:

`xbox/bench/out/content_compare_gameplay_gate/user_authorized_contended/`

Key files: `CONTENDED_RESULT.md`, `contended_summary.json`, `experiment.json`,
`run_six.log`, `arms/*/runs/*`.

The production content-watch call was retired back to the exact pre-SSE2
condition. The SSE2 helper, test seam, focused tests and microbenchmark remain
as experimental evidence, but are **not used by production**.

Tiny production diff:

```diff
-    } else if (!content_bytes_equal(content_watch[slot].snapshot, data, size)) {
+    } else if (memcmp(content_watch[slot].snapshot, data, size)) {
         memcpy(content_watch[slot].snapshot, data, size);
```

## Scope

The production content-watch equality operation is back on the CRT `memcmp`.
The global CRT `memcmp` and every other comparison in the renderer are
untouched. The SSE2 helper is now test-only.

Files changed:

- `xbox/src/d3d8gl/stubs.c`
  - retained the x86-64 SSE2 exact-equality helper as experimental test code;
  - retained the non-production test seam
    (`noble_d3d8_content_equal_test`);
  - restored the snapshot comparison at the `memory_watch_generation` call
    site to the exact pre-SSE2 `memcmp(...)` nonzero condition.
- `xbox/tests/d3d8gl_abi_test.c`
  - differential tests against the CRT reference;
  - exhaustive small-size/byte tests;
  - alignment matrices at selected sizes;
  - large 64 KiB/128 KiB boundary tests;
  - guard-page tests;
  - production `memory_watch_generation` mutation tests;
  - opt-in benchmark harness.

## Production status

`memory_watch_generation` no longer calls `content_bytes_equal`. It calls
`memcmp(content_watch[slot].snapshot, data, size)` exactly as before the SSE2
experiment.

`content_bytes_equal_sse2`, `content_bytes_equal` and
`noble_d3d8_content_equal_test` remain compiled only as test/experimental
surface. They are not called by production and add no runtime diagnostic
behavior.

## Comparison semantics

The retained test helper `content_bytes_equal` answers only "are all bytes
equal". It does not return the ordering of a first difference. It is exercised
only by the test seam and benchmark, not by production.

- 128-byte vector loop using eight 16-byte unaligned SSE2 loads, XOR/OR tree,
  then one `pcmpeqb`/`pmovmskb` equality check;
- 16-byte vector loop for the remainder;
- scalar byte tail;
- zero length returns equal without forming or dereferencing a source
  pointer;
- bounds are checked with `size - i` before every vector load, so no load
  extends past the declared range;
- `_mm_loadu_si128` is used throughout; no alignment assumption.

On non-x86-64 builds the helper falls back to `memcmp`. The host standalone
build is x86-64 and uses SSE2, which is baseline for x86-64. AVX2 is not used.

## Preserved behavior

- The production `memory_watch_generation` comparison is again the exact
  pre-SSE2 `memcmp(snapshot, data, size)` nonzero condition.
- The existing snapshot copy and generation increment are unchanged.
- The first and second `memory_watch_generation` calls in
  `xbox_textures.c` are unchanged; the second remains the post-protect race
  check.
- No cache, content-version, page-watch, snapshot, or locking policy was
  added or changed.
- In the retained test helper, every byte is inspected on an equal
  comparison. On inequality the vector loop exits at a 128-byte/16-byte
  block, just as `memcmp` exits at the first differing byte/qword; no hash,
  sampling, or probabilistic shortcut is used.
- The helper reads only the declared range. It does not validate, clamp, or
  redirect an invalid guest range.

## Tests

The ABI test now includes:

1. Differential equality against the CRT reference for the same inputs.
2. Sizes 0..512, four representative alignments, with every byte position
   toggled one at a time.
3. Every-byte checks at selected larger boundaries (1023, 1024, 1025, 4095,
   4096, 4097).
4. Full 0..31 x 0..31 alignment matrices at selected sizes, including
   65536 and 131072, with first/middle/last differences.
5. Large boundary sizes (65535, 65536, 65537, 131071, 131072, 131073) with
   representative alignments, every 16-byte block at 64 KiB, every 128-byte
   block at 128 KiB, and every byte in selected 128-byte windows.
6. Deterministic multiple-byte differences at both large sizes.
7. Guard-page tests: buffers end exactly at a `PAGE_NOACCESS` page for sizes
   0, tail sizes, 16/128-byte boundaries, 4095, and 4096. A read past the
   range faults the test.
8. Production-path mutation tests through `memory_watch_generation` at
   positions 0, 15, 16, 127, 128, and 4095, including restore mutations.

The test structure is deliberately bounded: no 1024 x 1024 offset-pair by
every-byte Cartesian product.

## Opt-in benchmark

The ABI test has an env-gated benchmark, off by default:

```bat
set RECOMP_CONTENT_COMPARE_BENCH=1
xbox\tools\build_d3d8gl.cmd test
```

It warms both paths with alternating modes, then measures 64 KiB and 128 KiB
equal/first/middle/last-difference cases with the SSE2 path and the CRT
reference. It prints `ns/call` and `MB/s`. It is not a pass/fail assertion,
does not measure FPS, and is not part of the default test output.

## Validation commands for the validator

No build or gameplay run was performed by this implementation.

```bat
xbox\tools\build_d3d8gl.cmd test
xbox\tools\build_d3d8gl.cmd gpu-test
xbox\tools\build_d3d8gl.cmd texture-test
ctest --test-dir xbox/build-tests -C Release --output-on-failure
```

Optional benchmark:

```bat
set RECOMP_CONTENT_COMPARE_BENCH=1
xbox\tools\build_d3d8gl.cmd test
```

After building, verify production `memory_watch_generation` calls
`memcmp(snapshot, data, size)` and that the retained test seam separately
contains the SSE2 `pxor`/`pcmpeqb`/`pmovmskb` path. The production path must
not require AVX/AVX2.

## Correctness limitations

- The retained helper is equality-only and is called only by the test seam,
  not production. It is not a replacement for `memcmp` where first-difference
  ordering matters.
- Production is back on the original `memcmp`; the content-watch state is
  still unsynchronized and neither existing race check was removed.
- The range must still be readable by the caller. The helper does not add a
  guard for invalid guest addresses; it only avoids reading outside the
  declared size.
- The vector test path is x86-64 SSE2; other builds use the CRT fallback.
- The benchmark uses `clock()` and is a diagnostic harness, not a
  performance gate or an FPS claim.
- The six-run comparison was contended; its mixed pairs and failed arm
  non-overlap do not support a clean performance result or a proven
  regression claim.
