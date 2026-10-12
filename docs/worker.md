# Offline worker adapter and backend dispatcher

`src/worker.hpp` provides `frameyap::Worker`: call `start(python, script, model,
threads=2, advanced_debug=false)` explicitly for the legacy Redux script, or
supply the optional `backend`, `manifest_dir`, `root` arguments for the manifest
dispatcher. Poll until `ready()`, then `submit(id, pcm)` and poll for one
`WorkerReply` (text or privacy-safe per-request error). One request at a time;
no queue, no capture and no input injection. `stop()` discards pending audio,
terminates/reaps its direct child (TERM, bounded 500 ms, then KILL),
and kills remaining processes in its dedicated owned worker group,
and is safe to repeat. Destruction stops it. `start()` returns without waiting
for model load; `poll()` reports warmup/load/crash/timeout/protocol errors by
throwing, then stops. Launch uses `posix_spawn`, safe with the host's OpenVR/SDL
threads, rather than running Python setup in a forked multithreaded child.
Caller must discard stale authorization/results after
cancellation; this component does not implement focus or delivery policy.

The adapter requires an existing absolute, owner-private `$XDG_RUNTIME_DIR`
(no symlink at the final component), creates its own 0700 `mkdtemp` directory,
and writes only `clip.raw` with `O_EXCL|O_NOFOLLOW`, mode 0600. Clips are
3200..320000 finite float samples in `[-1, 1]`, mono 16 kHz, stored as
little-endian IEEE float32 (0.2..20 s). The microphone adapter saturates finite
capture/resampler overshoot to this range without rescaling the rest of the
clip; NaN/infinity are rejected. `Worker::submit` independently rejects samples
outside this contract before writing a clip or request. Files are unlinked after replies or shutdown, and the
private directory is removed. Private clips are not encrypted against the
account owner/root; do not use an untrusted runtime directory. The caller
should pass a trusted interpreter and script. By default audio/transcripts are
not logged and child stderr is redirected to `/dev/null`. Request errors report
only a fixed stage (`audio`, `inference`, `response`) and built-in exception
category, never the exception message, arbitrary class name, traceback or text.
The native receiver allowlists those labels before displaying/logging them;
unknown/legacy errors remain generic.

The private pipes use unsigned LE32 payload lengths (1..65536), a one-byte
message type and, for requests/replies, unsigned LE64 request ID. `T` + ID
requests reading the fixed clip; `Y` means ready; `F` means load failure
(`M` for missing/mismatched pinned model or private clip directory, `I` for a
missing Python dependency, `P` for unavailable telemetry protection, `D` for runtime/model load failure); `R` + ID + UTF-8
text and `E` + ID + privacy-safe UTF-8 error are replies. Text is at most 4096
bytes. An unexpected or duplicate reply, wrong ID, extra frame, closed pipe
or oversized frame stops the worker. A correlated `E` reply or an invalid
transcript is a **request-level** failure: the Controller drops that clip,
keeps the ready model loaded and allows Record to retry. A broken protocol,
worker crash or explicit in-flight cancellation is different and can require a
reload. Warmup deadline is 120 s, transcription deadline 60 s; `poll()` must
be called regularly to enforce deadlines. It never initializes a headset or
starts a recording. There is no auto restart after a process failure.

Parakeet Redux is the **model** (`moondream/parakeet-redux`); `moondream` is
its Python inference package, not a second model or cloud endpoint. Kestrel is
a native dependency of that local runtime. `python/frameyap/worker.py` lazily
imports `moondream` only after explicit CLI startup, with HF/Transformers/Datasets
offline variables and bounded native thread-pool variables set before import. It uses
`md.photon("moondream/parakeet-redux", model_path=<absolute local directory>,
device="cpu", cpu_threads=threads)` and persistent
`transcribe(audio=<numpy float32>, sample_rate=16000)["text"]`.
Install an **isolated** Python runtime with the
moondream 2.4.0, kestrel 0.8.0 and compatible CPU dependencies; provide
preinstalled local weights from revision
`2bf128600aac4b16946f7ed8372e56117fe5e23b` and pass its directory
explicitly. The code verifies exact sizes and SHA-256 of weights/config/tokenizer
against `assets/backends/redux.json` via the shared, offline
`python/frameyap/model_files.py` schema/verifier before importing model libraries.
`frameyap --list-models` and `frameyap --check-model redux --model-dir /absolute/model`
(or `scripts/model-status.py`) read local manifests and optionally hash local
files; they never load the worker or download weights. For native `--run`,
`--backend ID`, `--model-store /absolute/store`, and
`--manifest-dir /absolute/manifests` are wired through the installed launcher
as explicit overrides; default selection is saved in `config.json` (`redux`). The dispatcher
`python/frameyap/backend_worker.py` validates a selected manifest and hashes
its local model, resolves an in-release relative Python/executable launcher
without a shell, then supervises one child over `frameyap-worker-v1` (ready,
correlated request/reply and failure frames). The launcher must accept
`{model_dir}` and `{clip_dir}` (optional `{threads}`); matching manifest and
pinned weights are **not** a runtime or automatically trusted new backend.
Only Redux is supplied today. Protocol stdout is isolated at the file-descriptor
level from third-party diagnostics. Thread limits cover Torch interop/native
pools and CUDA is not selected. Offline environment
flags do not prove every third-party internal is unable to access a network.
Runtime/build/tests perform no downloads; the separate explicit setup utility
`scripts/fetch-model.py` can provision the public pinned weights.

Native-only archives do not bundle the runtime; the explicit
`install.sh --install-runtime --yes` step pip-installs it into a user-local venv
(see [packaging](packaging.md) and [third-party notes](third-party.md)). No public runtime bundle has been released.
Limited ARM64 measurements were taken during development; they are not a claim
of complete headset acceptance.

## Moondream analytics

The persistent `moondream_analytics` boolean defaults to false, including when
absent from existing config. Settings requires a rendered disclosure and Confirm
Enable before saving true; failed enabling saves leave it off. Disabling takes
effect in-session even if saving fails, with an explicit next-launch warning and
retry control. The native controller passes the effective setting before its first
warmup and every restart. It overwrites inherited `FRAMEYAP_MOONDREAM_ANALYTICS`;
the manifest dispatcher forwards opt-in only to the known Redux Python launcher.
Direct worker invocation treats only environment value `1` as enabled; missing
or malformed values are off. Manual config edits are read on application restart.

With analytics off, the worker verifies moondream 2.4.0 and Kestrel 0.8.0 and the
private reporter coroutine interface, then replaces `PhotonReporter._flush_window`
with an async no-op before engine construction. Startup, periodic, shutdown and
nonrotating authentication-retry uploads share that method. It returns no account
standing, disabling cloud finetune authentication; FrameYap only uses the local
base Redux model. Incompatible versions/interfaces emit `F/P` and do not start
inference. Missing packages retain the existing `F/I` error. Enabled analytics
leave upstream behavior untouched, including its allowlisted API-base override
(`https://api-staging.moondream.ai`) and use of an inherited `MOONDREAM_API_KEY`
authentication header if present. FrameYap's local model needs no API key.
No installed package files are changed.

OFF persistence is deferred until after the old worker is stopped; a slow or
failed config write cannot keep the old enabled reporter running. Enabling
requires successful write, flush, close and rename before becoming effective.

Changing analytics cancels audio/review/queued delivery and immediately kills the
owned worker process group before closing its pipes, without graceful telemetry
hooks. The direct worker is reaped before replacement starts with the new choice.
Already sent/in-flight network requests cannot be recalled. Normal shutdown still
uses the existing bounded TERM/KILL cleanup. This is worker-scoped protection,
not an OS network sandbox or a guarantee about all native/transitive dependencies.

Audit source: [Kestrel 0.8.0](https://pypi.org/project/kestrel/0.8.0/) wheel
`kestrel/photon.py`, `kestrel/engine/core.py`, and
[moondream 2.4.0](https://pypi.org/project/moondream/2.4.0/) `moondream/photon_vl.py`.
The Kestrel wheel SHA-256 is
`15c65bf12983d347330082838d4c6a9678a7f00c0f32ae089d9ccad7482e62bd`.
Re-audit before changing the allowed versions: metadata/interface checks do not
detect a locally modified package retaining its original version.

Default offline tests cover config migration/persistence/failures, rendered opt-in
consent and cancellation, initial worker policy, toggles during pending work,
restart/cleanup, and version/interface failures. An optional exact-wheel test
executes only the upstream reporter with fake HTTP/runtime modules, checking
startup, idle periodic, shutdown and authentication retries in both states. It
never uploads, loads a model, or downloads dependencies:

```sh
FRAMEYAP_TEST_KESTREL_WHEEL=/path/to/kestrel-0.8.0-py3-none-any.whl \
  python3 -m unittest discover -s tests -p test_telemetry.py -v
```

## Advanced debugging

Explicitly set `"advanced_debug": true` in config or enable Settings → Advanced
debugging. It is off by default. A Settings change restarts the owned worker,
closes the microphone and discards current work/review; the replacement worker
warms normally. Turning it off stops detailed capture but **does not delete
previous logs**. Manual config edits take effect on application restart.

With this opt-in the worker receives `--advanced-debug`: native/model stdout and
stderr, sample counts, full exception tracebacks and recognized transcripts are
captured. **These logs may contain private speech, transcript text and local
paths. Inspect/redact before sharing; keep out of Git.** No raw audio archive is
created; normal temporary clips still expire after replies/cancellation.

Files: `$XDG_STATE_HOME/frameyap/worker-debug.log` (fallback
`~/.local/state/frameyap/worker-debug.log`) and `worker-debug.previous.log`.
Each debug worker start rotates the current log once; only these two files are
retained, each bounded to 4 MiB. At the limit a marker is written and further
output is drained/discarded for that worker session, not allowed to block it.
The directory is owner-private 0700 and logs are 0600. Unsafe paths, symlinks,
hardlinks or preexisting permissive files are refused with a visible error;
there is no fallback to public temporary files. The app's single-instance lock
is required to avoid competing rotation by multiple workers. Debug output never
shares the framed protocol stdout. Direct worker CLI use with `--advanced-debug`
writes to its caller's stderr; the native adapter supplies the private bounded sink.

The same opt-in also writes `delivery-debug.log` (rotated to
`delivery-debug.previous.log` when debugging is enabled, capped near 1 MiB) in
that directory: paced-delivery metadata only — start/sent byte counts and
offsets, lease/send errors, finish results and which focus-guard check failed.
It never contains transcript text.

Hardware-free tests run through CTest, including fake-child cancellation, short
injected warmup/request deadlines, duplicate/stale replies, malformed frames,
missing/hash-mismatched model files and symlink refusal. Default production
deadlines remain 120/60 seconds. Python tests never import actual model libraries,
record a microphone, download assets or initialize OpenVR.
