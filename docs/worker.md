# Offline Redux worker adapter (component, not an installed product)

`src/worker.hpp` provides `frameyap::Worker`: call `start(python, script, model,
threads=2)` explicitly, poll until `ready()`, then `submit(id, pcm)` and poll for
one `WorkerReply` (text or generic per-request error). One request at a time;
no queue, no capture and no input injection. `stop()` discards pending audio,
terminates/reaps **only its direct child** (TERM, bounded 500 ms, then KILL),
and is safe to repeat. Destruction stops it. `start()` returns without waiting
for model load; `poll()` reports warmup/load/crash/timeout/protocol errors by
throwing, then stops. Launch uses `posix_spawn`, safe with the host's OpenVR/SDL
threads, rather than running Python setup in a forked multithreaded child.
Caller must discard stale authorization/results after
cancellation; this component does not implement focus or delivery policy.

The adapter requires an existing absolute, owner-private `$XDG_RUNTIME_DIR`
(no symlink at the final component), creates its own 0700 `mkdtemp` directory,
and writes only `clip.raw` with `O_EXCL|O_NOFOLLOW`, mode 0600. Clips are
3200..320000 finite float samples, mono 16 kHz, stored as little-endian IEEE
float32 (0.2..20 s). Files are unlinked after replies or shutdown, and the
private directory is removed. Private clips are not encrypted against the
account owner/root; do not use an untrusted runtime directory. The caller
should pass a trusted interpreter and script. Neither audio nor transcripts
are logged; child stderr is redirected to `/dev/null`, so worker diagnostics
are deliberately generic.

The private pipes use unsigned LE32 payload lengths (1..65536), a one-byte
message type and, for requests/replies, unsigned LE64 request ID. `T` + ID
requests reading the fixed clip; `Y` means ready; `F` means load failure
(`M` for missing/mismatched pinned model or private clip directory, `I` for a
missing Python dependency, `D` for runtime/model load failure); `R` + ID + UTF-8
text and `E` + ID + generic UTF-8 error are replies. Text is at most 4096
bytes. An unexpected or duplicate reply, wrong ID, extra frame, closed pipe
or oversized frame stops the worker. Warmup deadline is 120 s, transcription
deadline 60 s; `poll()` must be called regularly to enforce deadlines. It
never initializes a headset or starts a recording. There is no auto restart.

`python/frameyap/worker.py` lazily imports `moondream` only after explicit CLI
startup, with HF/Transformers/Datasets offline variables and bounded native
thread-pool variables set before import. It uses
`md.photon("moondream/parakeet-redux", model_path=<absolute local directory>,
device="cpu", cpu_threads=threads)` and persistent
`transcribe(audio=<numpy float32>, sample_rate=16000)["text"]`.
Install an **isolated** Python runtime with the separately reviewed
moondream 2.4.0, kestrel 0.8.0 and compatible CPU dependencies; provide
preinstalled local weights from revision
`fad622f25f303105c20d70e201bcc477c88b620c` and pass its directory
explicitly. The code verifies exact sizes and SHA-256 of weights/config/tokenizer
against `model_files.py` before importing model libraries. Protocol stdout is
isolated at the file-descriptor level from third-party diagnostics. Thread limits
cover Torch interop/native pools and CUDA is not selected. Offline environment
flags do not prove every third-party internal is unable to access a network.
Runtime/build/tests perform no downloads; the separate explicit setup utility
`scripts/fetch-model.py` can provision the public pinned weights.

**Licensing blocker:** the observed kestrel-kernels 0.7.0 license requires a
separate M87 Labs agreement; do not treat wheel availability as permission for use
or bundling. See [third-party notes](third-party.md). No public runtime bundle has
been released. Limited ARM64 measurements are in the [POC record](evidence/poc-cpu-overlay-2026-09-24.md),
not a claim of complete headset acceptance.

Hardware-free tests run through CTest, including fake-child cancellation, short
injected warmup/request deadlines, duplicate/stale replies, malformed frames,
missing/hash-mismatched model files and symlink refusal. Default production
deadlines remain 120/60 seconds. Python tests never import actual model libraries,
record a microphone, download assets or initialize OpenVR.
