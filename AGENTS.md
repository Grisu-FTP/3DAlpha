# Working in 3DAlpha

Minecraft Java Alpha 1.1.2 for Nintendo 3DS; C++17, no exceptions/RTTI. Preserve
format/protocol compatibility. Use the session's actual shell and permissions.

## Read narrowly

- This is the shared agent entry point; `CLAUDE.md` points here. Do not read both again.
- Search `docs/task-map.md` for the task; it names entry points, tests and deeper references.
- Read `docs/current-work.md` for recent changes or continuation work. It is dated evidence,
  not proof that today's checkout passes tests.
- Search `docs/code-map.md` for modules and `docs/doc-index.md` for section ranges.
  **Do not open `docs/status.md` or large source/header files whole by default.**
- Search symbols first, then read their surrounding lines. Read only the relevant historical
  section when the implementation or a decision needs explaining.
- `docs/working-guide.md` has detailed commands/environment notes; `CONTRIBUTING.md` has
  the full review checklist. Consult relevant sections, not every document on every task.

## Essential constraints

- Preserve unrelated user changes, including untracked files. Check scoped `git status`/diffs;
  the working tree may contain substantial unfinished features.
- Core cannot include 3DS headers or `_3DS` conditionals. Version differences use generated
  feature constants or manifest-selected slots, never version comparisons or preprocessor branches.
- IDs and dimensions come from generated registries/config. Renderer uses render types;
  tick logic uses behaviours. Version data belongs in `data/`.
- No allocations in frame hot paths; no filesystem/decompression on core 0; no main-thread
  waits on workers. Keep revision checks on worker results. The 3DSX main-thread stack is 32 KB.
- Preserve unknown NBT. Test negative chunk coordinates for storage changes.
  **Copy real worlds before opening them:** storage open/close writes locks and level data.
- Derive Alpha behaviour from the local client jar/JVM tools, not memory or a wiki.
  Never ship jar contents or require player-supplied game data. Observe CONTRIBUTING's licensing rules.
- New source files start with a useful summary comment. New core logic needs host coverage.
  Shared code must build for supported versions. Hardware performance claims require hardware
  measurements; missing hardware does not block implementation or host/cross-build checks.

## Efficient verification and handoff

```sh
cmake --build build-host -j 4
./build-host/3dalpha_tests <name-substring>  # test CASE names, not filenames
./build-host/3dalpha_tests                  # full suite when warranted
make                                      # 3DS build, no deployment
python3 tools/gen_index.py                 # refresh navigation
python3 tools/gen_index.py --check
```

Run TSan for worker/cache/audio-thread changes (setup in `docs/working-guide.md`).
Keep long build/test output in `/tmp` logs; inspect failures and the final result. Do not
repeat passing checks without a relevant change or unresolved concern. Distinguish sanitizer
environment failures from test failures; follow session escalation rules.

After meaningful work, replace the relevant facts in `docs/current-work.md` (keep it short).
Record detailed milestones/measurements in the relevant `docs/status.md` section. Update the
task map only when navigation changes; regenerate indexes after file/heading/line-count changes.
Do not accumulate a second chronological status history in the short handoff.
