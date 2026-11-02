# Publication package

Current authorized GitHub sync: E59–E63 source, tests, designs and results, plus privacy cleanup, are included in the561-file source allowlist.64 evidence files,37,290 main records and43 figure pairs; keep model identifier deepseek-v4-flash-0731 and all negative results. Real service addresses/hosts, keys, personal configs and private logs are excluded. Earlier “no push” notes describe preparation stages and are superseded by this explicit sync authorization. Verify the remote main commit after a normal fast-forward push.

The source release guard also rejects embedded non-placeholder chat-completions URLs without a version prefix. Model identifiers remain allowed. Five release-guard suites pass; findings contain locations/rules only.

Current E62/E63 snapshot:64 evidence files,37,290 main records,43 figure pairs.50 cloud rounds and60 offline serialization blocks;20 task and30 tool auxiliary records separately. Preserve prior62 evidence and42 figure pairs. No stable latency/cache gain; retain4/8 regressions and serialization wall-time increase. cloud-observe artifacts exclude actual URL/key/profile/private traces/models; no push this round.

Historical E59–E61 snapshot:62 evidence files,37,180 main records,42 figure pairs.363 handler block records and153 independent probes are separate from3 real-cloud tasks. Preserve all prior59 evidence and41 figure pairs; no private endpoint/config/key/trace/model. Versioned-edit artifacts only, no Git push this round. Large full-write controls are C handlers, not CLI capabilities.

Current E58 snapshot:59 evidence files,36,664 main records,41 figure pairs. Read-only self-prediction adds functional acceptance and target-machine handoff only; no new performance samples or model hit-rate claim. Preserve all prior58 evidence files and41 figure pairs. Use fresh self-speculation artifact names; do not include model/runtime/build logs or push Git.

Current E56/E57 snapshot:58 evidence files,36,664 main records,41 figure pairs. Old56 evidence files and40 pairs stay byte-identical. E55 bound deployment targets differed; preserve that limitation, aborted warmups and failed grouped-rule candidate. Optional one-task PCH is build-command work, not inference acceleration. Fresh sandbox-cpp artifacts only; no Git push.

Historical E54/E55 snapshot:56 evidence files,36,214 main rows,40 figure pairs and70 separately labeled direct-API records. All prior54 evidence files and39 pairs remain byte-for-byte unchanged. Same-Xcode binding is explicit, grants no permissions, and is separate from inference optimization. Preserve failed controls and residual pairs. Use fresh sandbox-cc-final exports; no Git push.

The historical E53 host-environment and OS-boundary snapshot contains: 54 evidence files, 35,980 main rows and 39 figure pairs. E52 is pre-fix; narrowed Seatbelt sysctl rules alone did not hide the parent environment. Current cxf protects and verifies its own startup environment, not all same-UID processes or argv. The new 230-row overhead experiment and all failures are separate. Preserve the prior 53 evidence files and 38 pairs; the test-sandbox-host exports remain the artifacts for that stage. No Git push.


E52 is retained as the historical initial macOS test.run isolation experiment: 230 overhead rows (44 warmups, 93 evaluation pairs), all negative values, a rejected localhost filter, and the later parent-environment gap. Its snapshot contains 53 evidence files, 35,750 main records and 38 figure pairs. It is superseded by E53; neither stage claims Linux/Windows sandbox acceptance or model/cloud speedup.

This directory is the public results entry point. Use [README.md](README.md) or [README.zh-CN.md](README.zh-CN.md) for GitHub presentation. The original JSON files in `docs/evidence/` remain the numerical source of truth; the reports, figures and CSV are derived views.

## Release contents

The exact allowlist is [publication-manifest.json](publication-manifest.json), with size and SHA-256 for every file except the manifest itself:

- Bilingual root project READMEs, MIT license, source release guide, current adaptation inventory, allowlisted design contracts/audit, English/Chinese benchmark reports, methodology, maintenance/change records and this publishing note. Planned designs are explicitly unimplemented.
- Sixty-four evidence JSON files, separated into raw/functional/sanitized-summary/reported-only classes.
- Full indexed raw records, tidy aggregate metrics, per-cell batch/context errors, frozen-calibration training/validation/anchor CSV, Kalman summary/per-cell/innovation/replay CSV, actual-tool task/summary/paired-difference/restore-component CSV, restore stage/ETA-audit/control/summary CSV, complete-task additive accounting and indexed historical-pair audit CSV, deletion resource/child-phase/last-close controls and executable-launch/incremental task CSV, resident tool-flow, real-process streaming call/task/pair/arrival CSV and actual CLI output task/stage/pair/overhead CSV, producer CPU/EOF/worker attribution and500 separately labeled exit-probe diagnostics, Metal per-command/per-wait and scope partitions, notification coverage, bare/probe/batch matched tasks/pairs, validation matrix, catalog, integrity checks and metadata-only run inventory.
- Forty-three vector SVG figures and their PNG previews.
- Standard-library report/check/export script and the ReportLab figure script.

Generate a deterministic, offline results bundle from repository root:

```sh
python3 tools/build_benchmark_report.py --check --export build/cxf-benchmark-results-20261102-public-model.zip
```

Extract it while preserving directories; both language reports and their local links work offline. The bundle is a **data/report release**, not the cxf executable or a source-code/model distribution. It does not require model weights or access to this machine’s build directory. Recomputing reports works from the bundle; rerunning inference requires the complete project and matching dependencies.

## Privacy boundary

Public model identification is allowed: `deepseek-v4-flash-0731` is the requested model alias for E60/E62. Per-round and per-task E62 CSV include a `model` column; source evidence, numerical values and negative results remain unchanged. The alias does not verify remote weights.

Do not publish real service URLs, host/IP hints, keys, authorization values, account identifiers, personal configuration paths, or the provenance of an individual's API configuration. Use placeholders in reproduction commands. Generic API schemas, loopback fixtures, protected-directory names, model aliases and the HTTP/TLS measurement boundary are technical documentation, not deployment credentials. Scan the complete source/result allowlists and packaged bytes; report only matching filenames/rules, never secret values.

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

E50 adds functional tool/environment/local-helper acceptance only. The previous50 evidence files and37 figures remain unchanged historical measurements. Updated source and CLI behavior has no new performance result.


## E51 local functional update

E51 adds uniform protected-file access and default-deny cloud egress. Twelve synthetic-secret HTTP suites and445 policy checks pass under sanitizers, with the prior eight foundation suites and Native/14K/Stream/output compatibility. The prior51 evidence files and37 figure pairs remain byte-for-byte unchanged;52 evidence files still contain35,520 historical main records. No real cloud, universal DLP, process sandbox or new performance result. Export new agent-data-policy artifacts; do not overwrite prior releases or push Git automatically. See [D-DATA](../design/data-policy-v1.md).
