# Agent Instructions (tea-live-subtitle)

The workflow rules (who dispatches, worktree + PR, acceptance, CI failure log) live in the server repo: `/Users/c2leb/Codes/tea-asr-service/AGENTS.md`. Read it first. This file only lists the checks specific to this plugin.

## Before pushing: run what CI runs

Every red CI run sends the user a GitHub failure email. All of the failures below happened, and all of them can be caught locally.

1. **Protocol tests.** Extract the commands from `.github/workflows/protocol-tests.yaml` unchanged and run them with `RUNNER_TEMP` set:
   - `sed -n '25,42p' .github/workflows/protocol-tests.yaml | sed 's/^ *//' > ci.sh`
   - `RUNNER_TEMP=<scratch> bash -e ci.sh`
   - Check the line range against the yaml first.
2. **C code must be strict C11.** Linux CI uses `-std=c11 -Werror`, which rejects POSIX-only calls. Use `bstrdup`/`bmem`, not `strdup`.
3. **No deprecated OBS APIs.** The Linux plugin build turns deprecation warnings into errors; for example, use `obs_properties_add_button2`, not `obs_properties_add_button`. Compile locally with `-Werror -Werror=deprecated-declarations`.
4. **Headers shared with C++ must be C++17-clean for MSVC.** Don't use designated initializers (`{ .x = ... }`) in any `.h` that a `.cpp` includes. To check, compile a stub `.cpp` that includes the header: `c++ -std=c++17 -Wpedantic -Wc++20-designator -fsyntax-only -Isrc -Itests/stubs stub.cpp`.
5. **Formatting.** Run `uvx --from clang-format==19.1.1 clang-format --dry-run -Werror <file>` one file at a time; passing several files per call fails in the uvx wrapper. Run gersemi only if CMake files changed.
6. **End-to-end.**
   - Build: `cmake -S tests/e2e -B <scratch> -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" && cmake --build <scratch>`
   - Run: `python3 tests/e2e/run_e2e.py --driver <scratch>/asr-client-e2e --service-dir /Users/c2leb/Codes/tea-asr-service`
   - Every scenario must pass.
7. **Caption behaviour changes.** Also run the replay tool in `tests/replay/` against real-model traces (see its README). Visible text of an open segment must never shrink.

## Never

- Install into `~/Library/Application Support/obs-studio/plugins/` or operate the user's OBS.
- Connect to or kill the user's live server on port 8327.
