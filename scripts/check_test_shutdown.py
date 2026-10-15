#!/usr/bin/env python3
"""Catch a unit test that starts the n00b runtime and never shuts it down.

n00b#527 was the archetype.  test_alloc_interior_large called n00b_init and
returned from main without n00b_shutdown, so the process entered libc's exit
teardown with the runtime still up -- MEASURED at 5 live threads, because
n00b_init starts the default conduit's IO service.  That raced as an
intermittent SIGBUS on the macOS CI runner, reddening unrelated PRs.

The fingerprint is distinctive and worth recognising: every assertion prints
PASS, the test's own closing line prints, and THEN the process dies with no
n00b crash-handler output.  A fault during exit teardown looks exactly like
that, because the handler's state is going away at the same time.

WHY A SCRIPT RATHER THAN A RUNTIME ASSERT.  The failure is a race, and it is
one this hardware does not lose: 360 runs at 12-way parallelism on a 16-core
box reproduced nothing, while a 3-core CI runner hits it.  A check that only
fires when the race is lost is not a check.  This is a static property --
"main calls n00b_init but not n00b_shutdown" -- so read it off the source and
it holds on every machine.

THE ALLOWLIST IS DEBT, NOT POLICY.  98 other tests have the same defect.
Fixing them all in one change would be a large, unreviewable diff across the
suite, and each needs its own look (a few may genuinely exit early on
purpose).  So they are listed here: the list caps the problem at its current
size, makes it countable, and is meant to be burned down.  Shortening it
needs no change to this script.

Exit status is nonzero when a test outside the allowlist is missing the call,
or when the allowlist names a file that no longer has the defect (so the list
cannot rot).  --self-test verifies the check can actually fail, because a
guard that cannot fail reports success forever -- the failure mode that let
n00b#444 through.
"""

import argparse
import re
import sys
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent.parent / "test" / "unit"

# Tests that call n00b_init/n00b_init_simple and never n00b_shutdown,
# as of n00b#527.  Debt:
# shrink this, do not grow it.  A file listed here that gets fixed makes this
# script fail until the entry is removed, which is how the list stays honest.
ALLOWLIST = {
    "test_arena_segment_stw_race.c",
    "test_attest_arena_lifecycle.c",
    "test_attest_base64.c",
    "test_attest_cli_extract.c",
    "test_attest_cli_mark.c",
    "test_attest_cli_sign.c",
    "test_attest_cli_unmark.c",
    "test_attest_cli_verify.c",
    "test_attest_dsse_roundtrip.c",
    "test_attest_envelope_sign.c",
    "test_attest_envelope_signatures_roundtrip.c",
    "test_attest_envelope_verify.c",
    "test_attest_envelope_verify_signature.c",
    "test_attest_err_str.c",
    "test_attest_extract_from_artifact.c",
    "test_attest_mark_artifact.c",
    "test_attest_mark_attestation_json.c",
    "test_attest_mark_elf_e2e.c",
    "test_attest_mark_macho_e2e_macos.c",
    "test_attest_mark_pe_e2e.c",
    "test_attest_module.c",
    "test_attest_monocypher_smoke.c",
    "test_attest_oci_arena.c",
    "test_attest_oci_client_resolve.c",
    "test_attest_oci_redirect_policy.c",
    "test_attest_oci_size_caps.c",
    "test_attest_signer_arena.c",
    "test_attest_signer_keyid.c",
    "test_attest_signer_release.c",
    "test_attest_signer_resolve.c",
    "test_attest_signer_sign.c",
    "test_attest_statement_roundtrip.c",
    "test_attest_verifier_arena.c",
    "test_attest_verifier_keyid.c",
    "test_attest_verifier_resolve.c",
    "test_aws_dynamodb.c",
    "test_aws_s3_contract.c",
    "test_aws_s3_integration_smoke.c",
    "test_aws_sts.c",
    "test_chalk_macho_resign.c",
    "test_chalk_module.c",
    "test_chalk_pe_resign.c",
    "test_clickhouse_module.c",
    "test_der_encode.c",
    "test_errno_str.c",
    "test_gc_large_alloc_churn.c",
    "test_gc_scan_bound.c",
    "test_gc_scan_bound_hysteresis.c",
    "test_gf256.c",
    "test_grammar_baked_equiv.c",
    "test_marshal_scratch_bound.c",
    "test_mmap_probe_cache_bound.c",
    "test_n00b_eval.c",
    "test_naudit_baseline.c",
    "test_naudit_blame.c",
    "test_naudit_cli.c",
    "test_naudit_cli_json.c",
    "test_naudit_engine.c",
    "test_naudit_engine_bulk.c",
    "test_naudit_engine_legacy.c",
    "test_naudit_engine_malloc.c",
    "test_naudit_engine_query_mode.c",
    "test_naudit_exemption.c",
    "test_naudit_filter.c",
    "test_naudit_filter_e2e.c",
    "test_naudit_guidance.c",
    "test_naudit_languages.c",
    "test_naudit_module.c",
    "test_naudit_parse_string.c",
    "test_naudit_preprocess.c",
    "test_naudit_reference_guidance.c",
    "test_naudit_rewrite.c",
    "test_naudit_signing.c",
    "test_naudit_signing_ux.c",
    "test_naudit_trust_root.c",
    "test_path_antipattern_sweep.c",
    "test_path_xdg.c",
    "test_pe_cert_table_emit.c",
    "test_pkcs7_signed_data.c",
    "test_pool_page_cache.c",
    "test_qr_codewords.c",
    "test_qr_matrix.c",
    "test_qr_render.c",
    "test_reed_solomon.c",
    "test_rocs_async_seal.c",
    "test_rocs_batch_ingest.c",
    "test_rocs_ingest.c",
    "test_rocs_large_shard_seal.c",
    "test_rocs_module.c",
    "test_rocs_recovery_journal.c",
    "test_rocs_seal_bench.c",
    "test_rocs_shard.c",
    "test_rocs_shard_lifecycle.c",
    "test_static_grammar_image.c",
    "test_static_grammar_image_unsupported.c",
    "test_x509_walk.c",
}

INIT_RE = re.compile(r"\bn00b_init(_simple)?\s*\(")
SHUTDOWN_RE = re.compile(r"\bn00b_shutdown(_simple)?\s*\(")


def strip_comments_and_strings(src: str) -> str:
    """Drop // and /* */ comments and string literals.

    Without this, a test that merely MENTIONS n00b_shutdown in a comment --
    which is exactly what a test documenting why it does not shut down would
    do -- reads as compliant.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            i = src.find("\n", i)
            if i < 0:
                break
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and src[i] != quote:
                i += 2 if src[i] == "\\" else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def offenders(files):
    """Files that start the runtime and never shut it down."""
    bad = []
    for path in sorted(files):
        code = strip_comments_and_strings(path.read_text(errors="replace"))
        if INIT_RE.search(code) and not SHUTDOWN_RE.search(code):
            bad.append(path.name)
    return bad


def self_test() -> int:
    """The check must be able to fail, and must not be fooled by a comment."""
    import tempfile

    cases = [
        ("offender", "int main(){ n00b_init(&rt, argc, argv); return 0; }", True),
        ("compliant",
         "int main(){ n00b_init(&rt, argc, argv); n00b_shutdown(); return 0; }",
         False),
        ("shutdown_simple",
         "int main(){ n00b_init(&rt, argc, argv); n00b_shutdown_simple(); }",
         False),
        ("comment_only",
         "/* we never call n00b_shutdown() here */\n"
         "int main(){ n00b_init(&rt, argc, argv); return 0; }",
         True),
        ("string_only",
         'int main(){ n00b_init(&rt,argc,argv); puts("n00b_shutdown()"); }',
         True),
        ("init_simple_offender",
         "int main(){ n00b_init_simple(argc, argv); return 0; }", True),
        ("init_simple_compliant",
         "int main(){ n00b_init_simple(argc,argv); n00b_shutdown(); }", False),
        ("no_runtime", "int main(){ return 0; }", False),
    ]

    failures = 0
    with tempfile.TemporaryDirectory() as td:
        for name, src, should_flag in cases:
            p = Path(td) / f"test_{name}.c"
            p.write_text(src)
            flagged = bool(offenders([p]))
            ok = flagged == should_flag
            print(f"  [{'PASS' if ok else 'FAIL'}] self-test {name}: "
                  f"flagged={flagged} expected={should_flag}")
            failures += not ok

        # The file SELECTION is part of the guard too: a bench that is never
        # looked at cannot be flagged, which is how the n00b#527 fix missed two.
        sel = Path(td) / "selection"
        sel.mkdir()
        for name in ("test_a.c", "bench_b.c", "helper_c.c"):
            (sel / name).write_text("int main(){ n00b_init(&rt,argc,argv); }")
        picked = sorted(f.name for f in candidate_files(sel))
        ok = picked == ["bench_b.c", "test_a.c"]
        print(f"  [{'PASS' if ok else 'FAIL'}] self-test selection: "
              f"picked={picked}")
        failures += not ok

    if failures:
        print(f"\nself-test: {failures} case(s) failed -- the guard is broken")
        return 1
    print("\nself-test: the guard can fail, and comments/strings do not fool it")
    return 0


# Benchmarks are executables too: they call n00b_init in their own main and
# are registered as tests. Globbing only test_*.c let bench_rocs_catalog_rewrite
# and bench_rocs_retention_pinned ship without n00b_shutdown, and the first
# crashed main's Windows lane with an access violation during exit teardown.
CANDIDATE_GLOBS = ("test_*.c", "bench_*.c")


def candidate_files(directory: Path) -> list:
    return sorted({f for g in CANDIDATE_GLOBS for f in directory.glob(g)})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true",
                    help="verify the check can actually fail, then exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    files = candidate_files(TEST_DIR)
    if not files:
        print(f"ERROR: no tests found under {TEST_DIR}", file=sys.stderr)
        return 1

    bad = set(offenders(files))
    new = sorted(bad - ALLOWLIST)
    fixed = sorted(ALLOWLIST - bad)

    if new:
        print("ERROR: these tests call n00b_init and never n00b_shutdown.\n"
              "       The process then enters libc exit teardown with the\n"
              "       runtime's threads still live, which raced as an\n"
              "       intermittent SIGBUS on the macOS runner (n00b#527).\n")
        for name in new:
            print(f"  {name}")
        print("\nAdd n00b_shutdown() before main returns.")

    if fixed:
        print("\nERROR: these are in the allowlist but no longer offend.\n"
              "       Remove them -- a stale allowlist hides the next one.\n")
        for name in fixed:
            print(f"  {name}")

    if new or fixed:
        return 1

    print(f"ok: {len(files)} tests and benches checked, "
          f"{len(ALLOWLIST)} known offenders remaining (n00b#527 debt)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
