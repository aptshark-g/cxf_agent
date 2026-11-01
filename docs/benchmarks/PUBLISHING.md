# Publication package

This directory is the public results entry point. Use [README.md](README.md) or [README.zh-CN.md](README.zh-CN.md) for GitHub presentation. The original JSON files in `docs/evidence/` remain the numerical source of truth; the reports, figures and CSV are derived views.

## Release contents

The exact allowlist is [publication-manifest.json](publication-manifest.json), with size and SHA-256 for every file except the manifest itself:

- Bilingual root project READMEs, MIT license, source release guide, current adaptation inventory, allowlisted design contracts/audit, English/Chinese benchmark reports, methodology, maintenance/change records and this publishing note. Planned designs are explicitly unimplemented.
- Fifty evidence JSON files, separated into raw/functional/sanitized-summary/reported-only classes.
- Full indexed raw records, tidy aggregate metrics, per-cell batch/context errors, frozen-calibration training/validation/anchor CSV, Kalman summary/per-cell/innovation/replay CSV, actual-tool task/summary/paired-difference/restore-component CSV, restore stage/ETA-audit/control/summary CSV, complete-task additive accounting and indexed historical-pair audit CSV, deletion resource/child-phase/last-close controls and executable-launch/incremental task CSV, resident tool-flow, real-process streaming call/task/pair/arrival CSV and actual CLI output task/stage/pair/overhead CSV, producer CPU/EOF/worker attribution and500 separately labeled exit-probe diagnostics, Metal per-command/per-wait and scope partitions, notification coverage, bare/probe/batch matched tasks/pairs, validation matrix, catalog, integrity checks and metadata-only run inventory.
- Thirty-seven vector SVG figures and their PNG previews.
- Standard-library report/check/export script and the ReportLab figure script.

Generate a deterministic, offline results bundle from repository root:

```sh
python3 tools/build_benchmark_report.py --check --export build/cxf-benchmark-results-20261101-initial-publish.zip
```

Extract it while preserving directories; both language reports and their local links work offline. The bundle is a **data/report release**, not the cxf executable or a source-code/model distribution. It does not require model weights or access to this machine’s build directory. Recomputing reports works from the bundle; rerunning inference requires the complete project and matching dependencies.

## Privacy boundary

The allowlisted package excludes `agentctl.json`, `.workbuddy/`, sibling projects, raw HTTP/config/request traces, engine logs, model weights, native binaries, KV snapshots and local runtime paths. Imported historical data was reduced to measured fields and hashes; private request logs were not copied. Static checks inspect the release for common credential strings, private endpoint URLs and home-directory paths. Do not print secret matches in a review log.

This remains a scoped data-release check. The 2026-11-01 [source release preparation](../RELEASING.md) separately checks the allowlisted public working tree and Git-visible candidate scope. Local configuration and ignored/private data remain excluded; static patterns do not prove the absence of every possible secret. The clean source export excludes Git objects; future pushed history requires its own review. No Git staging, commit, remote modification or push is performed by these scripts.

## Maintenance rules

The [current adaptation inventory](../adaptation-status.md), [detailed maintenance workflow](MAINTENANCE.md) and [change record](CHANGELOG.md) must stay aligned with later implementation and measurement changes.

1. Keep old evidence immutable. Add a new dated evidence file for a new trial and assign a new experiment/version ID.
2. Record hardware/backend, model/template/runtime hashes, source hashes, exact baseline, warmup/order, samples, units, exclusions, and failures.
3. Preserve unsuccessful controls and workload-dependent regressions; never replace the fair cached baseline with a cold baseline for a headline.
4. Rebuild both language reports and metrics from the same data. Keep physical storage, logical references, temporary heap, RSS and KV allocation distinct.
5. Re-render charts when their inputs or chart script change; inspect PNG previews. Regenerate the manifest and run `--check` before exporting.
6. Label missing provenance and unmeasured claims. Small-n ranges are descriptive, never p95/p99 or proof of stability.

The repository owner explicitly authorized the first GitHub source/data upload on 2026-11-01. This is separate from these report scripts, which never perform Git operations. The source manifest governs the actual commit; original performance data and negative results retain their measured revisions.
