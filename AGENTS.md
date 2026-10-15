# Agent rules

This file is the contract for anyone (human or agent) changing a
repository that uses it. It is self-contained: do not look up an
external skill to commit, and do not invent a second toolchain
beside mise.

The premises, working and test rules, mise rules, documentation rules,
and commit rules below are **not** project-specific. Copy them as-is. Architecture, layout, and
constraints of *this* tree live only under **This project**.

## Premises

- **Performance is a reason to change.** If a change is correct and
  makes sense, do it even when the measured gain is 0.01 ms. Do not
  skip a worthwhile optimisation because the delta is small. Measure
  when the cost of being wrong is high; do not use "nobody can
  perceive it" as a veto.
- **Zero dead code.** Delete unused functions, types, imports, flags,
  and files. Do not leave a replacement beside the thing it replaces.
- **Zero duplicated code.** One implementation of a behaviour. Extract
  or share rather than copy.
- **Zero legacy code.** No compatibility shims, deprecated aliases,
  dual paths, or "keep the old one until later" leftovers. Finish the
  cutover in the same change that introduces the new path.
- **Comments state the code.** A comment names what the next lines
  do, or a constraint the type system cannot. It is not a tutorial,
  a history, or a design essay.
- **Documentation lives in `docs/`.** Guides, measurements, language
  traps, and "how this is meant to be used" go there — not in
  comments, not in commit messages as a substitute for a doc, and not
  in this file except as rules.
- **English only.** Code, comments, commit messages, `docs/`,
  `README.md`, this file, mise task names and descriptions: English.

## Working

- **No spin-busy.** Do not rebuild or rerun checks after each edit.
  Finish the change and make it work against the narrowest test that
  covers it, then run the full verification once (**This project**
  names it). A session that ends in several commits verifies the last
  one.
- **Pictures are for pull requests.** Every picture is redrawn and
  checked once, before a branch is merged. Between merges, draw only
  the pictures a change touches.
- **Clean up after every run.** A build, check or benchmark removes
  what it started, whether it passes, fails or is interrupted:
  processes, sessions, sockets, temporary files. Keep only the result
  it was meant to produce.

## Tests

- **Wait for evidence, not time.** A test waits for the state, output,
  file or signal that proves the work finished. A deadline only bounds
  a failure. Never sleep for a settle time and never assert elapsed
  time.
- **Disposable environment.** A check runs with fresh `HOME`, `XDG_*`
  directories and `TMPDIR` under `/tmp`, so the user's desktop
  settings, accounts and files cannot change its result, and removes
  them when it ends. A check that needs the real session says so in
  its description and touches it only through what it starts.
- **A behaviour test is proven by mutation.** Break the rule it checks
  — remove the guard, invert the condition, return the old value — and
  run the narrowest test: the test that names that behaviour must
  fail, not a crash or a timeout elsewhere. A fix's test must fail on
  the code before the fix. A surviving mutation is a missing test, or
  a path whose result does not change (a performance path: prove it
  with a measurement). Revert every mutation; never loosen an
  assertion or add a delay to make one fail. Transcript and picture
  checks, toolchain checks, benchmarks and behaviour outside the code
  are not proven this way.
- **A new law is mutated once.** Where properties are proven, breaking
  the code a new law covers must make its proof fail; otherwise the
  law says less than intended.

## Tooling: mise

mise owns the toolchain and every project command. Pin tools in
`mise.toml`. Run `mise install` before work. Invoke tools through
mise (`mise exec -- …` or, once defined, `mise run <task>`). Do not
call a system compiler or helper that is not the pinned one. Do not
add a parallel runner (Make, npm scripts, ad-hoc shell wrappers)
that duplicates a mise task.

### Tasks

Work lives in **scripts**, not in `mise.toml` bodies and not in
`mise-tasks/`. Each script is one job. mise **consumes** those
scripts: a `[tasks.name]` entry names the script, sets
`description` / `depends` / `env` / `sources` / `outputs` /
`usage` / `confirm`, and runs it. Git hooks and any other caller
run the same file. Do not duplicate the job as inline `run =`
shell, a file task, and a script.

- Put scripts under `scripts/`. Make them executable, with a
  shebang. Prefer `MISE_PROJECT_ROOT` when the script must know
  the repo root; otherwise relative paths from the project root
  (mise sets cwd to the directory of `mise.toml`).
- **Atomic:** one script, one responsibility. Compose larger work
  with `depends` (may run in parallel) or a `run` **array** of
  script invocations (sequential; a failure stops the rest,
  `set -e` for `sh`/`bash`/`zsh`). Aggregators (`check`, `bench`,
  `build`) only wire scripts together. The documented entry is
  `mise run <name>`; a hook calls `scripts/<name>`, not a second
  copy of the logic.
- **Idempotent:** running the same script twice with the same
  inputs leaves the tree in the same good state. Safe to re-run
  from a hook, CI, or `mise run`.
- Give every task a `description`. Use `alias` only when the short
  name is used often. Confirm destructive tasks with `confirm`.
- Declare `sources` and `outputs` when a task is skippable if
  inputs have not changed (builds). Use `sources` alone with
  `mise watch`.
- Arguments: a `usage` spec on the TOML task. Values arrive as
  `usage_*` environment variables. Do not use the deprecated Tera
  `arg()` / `option()` / `flag()` helpers.
- Do not add file tasks under `mise-tasks/` (or the other mise
  task directories). Do not keep a TOML task and a file task for
  the same name.
- List and inspect with `mise tasks ls` and
  `mise tasks info <name>`. Run with `mise run <name>`. Do not
  tell the user to `mise exec` a one-off that already has a task.
  Do not tell them to invoke `scripts/` when a task exists, except
  from git hooks and other non-mise entry points that must call
  the script directly.

## Documentation

Documentation is written for somebody using the project, one question
at a time, and kept true by the same tools that build it.

### The pages

- `README.md` is the front door, written for somebody installing the
  released program: what the project is in one paragraph, pictures of
  it working, Install (the release's installer, never development
  commands), Start here, the big ideas as short bold-led paragraphs
  each linking its page, the interface (keys, with a picture), What it
  does not do, Known limitations, What is broken (only while a defect
  is known), one line sending contributors to the design page,
  Requirements, license. Development and build commands live in the
  design page, not here.
- `docs/README.md` is the map: every page in the order somebody meets
  it, one line each on the question it answers, the shortest useful
  path, a "when you know what you want" table, and the conventions
  every page shares.
- `docs/how-to-*.md` holds one task per page. Each opens with
  "**The question:**" and says whether it is enough on its own, then
  "Before you start", then numbered steps, each with the command or
  the action and its real output, and ends with what can go wrong and
  where to go next. A page never explains a task that has its own page;
  it links it.
- `docs/screens.md` walks every screen in the order of use, with
  pictures, keys, and the screens that have none. It says how its
  pictures are made and what in them is pinned.
- `docs/cli.md` is generated from the program's own command
  definitions by a mise task, never edited by hand; `mise run check`
  fails when it is stale.
- `docs/what-does-not-work.md` exists only while a known defect is not
  fixed: every such defect, numbered, each with **What happens.** and
  **What to do in the meantime.**, and whether it was seen running or
  read in the code, with a "What is broken" section in `README.md`
  sampling it. No other page names a defect that is not there. When
  the last defect is fixed, the page and the README section go; when a
  known defect is left unfixed, both come back in the same change.
- Limits of the platform or of a provider are not defects: `README.md`
  lists them under "Known limitations", and each is said again on the
  page where somebody meets it.
- `docs/design.md` is for contributors: the model, the architecture,
  state on disk, protocols, packaging, what it is and is not, what was
  measured. Its sections are numbered and the numbers do not move: code
  and pages cite them as `docs/design.md §N`.

### Pictures

Pictures in `docs/img` are generated from the real program by a mise
task, against an isolated instance seeded with synthetic, versioned
fixtures — never the developer's own data, accounts or keys. Name them
`NN-<what>.png` in the order the screens page walks them. Look at every
picture a change produces. A picture that cannot be generated says so,
and where it came from.

### Discipline

- The documentation changes in the same change as the behaviour it
  describes: a new or changed command, screen, setting or defect
  updates its pages, pictures and `docs/cli.md` in that change.
- Facts come from the code or from running it. Outputs are real
  outputs. What was measured is said to be measured; what was only
  argued or read is said so in those words.
- Plain, short sentences: say what happens and what to do. No
  marketing, no promises of future work outside the defects page.
- Placeholders are named as placeholders the first time they appear.
- Every relative link, image and anchor resolves; `mise run check`
  runs a link check that fails when one does not.

## Commits (okt-task-commit)

Agents draft and create commits. The human owns publication: **never
`git push`**. When the tree is clean, say that a push is ready.

These rules inline the okt-task-commit playbook, the Conventional
Commits 1.0.0 grammar used with it, and the conventional-commits law.
There is nothing extra to fetch.

### Procedure

1. Read `git status` and `git diff --cached`. If nothing is staged,
   read the unstaged diff (`git diff` and untracked files).
2. Group hunks into **one intent per commit**. Split mixed trees with
   non-interactive staging only: `git add <path>` and
   `git restore --staged <path>`. Do not use `git add -p` or
   `git add -i`.
3. Derive **scope** from the paths touched (package, directory, or
   feature slug).
4. Draft the message (grammar below). Show every draft to the user
   **before** `git commit`.
5. Commit. Do not push. When the working tree is clean, suggest the
   user push when ready.

### Message grammar

```
<type>(<scope>)!: <subject>

<body>

<footer(s)>
```

- **type** (required): `feat`, `fix`, `docs`, `refactor`, `chore`,
  `test`, `build`, `ci`, `perf`. `feat` and `fix` are
  SemVer-significant. One type per commit: a feature and a fix are
  two commits.
- **scope**: noun in parentheses, from the paths. Omit only when no
  scope is honest.
- **!**: immediately before `:` for a breaking change
  (`feat(api)!: …`). Pair with a `BREAKING CHANGE:` footer.
- **subject**: English, imperative mood ("Add" not "Added"), ≤50
  characters, no trailing period, lowercase after the colon.
- **body** (optional): wrap at 72 columns. Explain the *why* the
  diff does not. Not a paste of the patch.
- **footers**: `Token: value` lines (`Refs: #123`,
  `BREAKING CHANGE: drops X`).

Never attribute a commit to an agent: no `Co-Authored-By: <model>`,
no `Generated with <tool>`, no model name in trailer or body. The
human running the session is the author. No opt-out. Other
`Co-Authored-By` trailers only if the user asked for that person in
this conversation.

Bad: `feat: add filter and fix duplicate insert` (two intents).
Good: `feat(filter): add priority option` then
`fix(insert): prevent duplicates`.

## This project

omafiles puts tags on files without moving, renaming or wrapping them.
One text index under `$XDG_DATA_HOME/omafiles/` is read and written by
two front ends: the `omafiles` command line and the `omafiles-studio`
Qt Quick window, which is also a full file manager and the desktop's
portal file chooser.

    files on disk  <──  index (JSONL)  <──  omafiles CLI
                              ^
                              └──────────  omafiles-studio  ──>  helpers

`src/core/` is the library both front ends link: the index store,
tags, roots, views, names, paths, the Trash reader and the CPU budget.
`src/cli/` is the command line, `src/studio/` the window (C++ models
and `src/studio/qml/`). Work that can hang, crash or grow on untrusted
input runs in its own process: `src/archive/` (libarchive) and
`src/devices/` (the GIO volume monitor, which links GLib/GIO only)
speak line-delimited JSON over stdin and stdout; `src/preview/`
(quick preview) sends length-prefixed binary packets over stdout and
reads newline commands on stdin. `docs/design.md` describes the
model, state on disk and these protocols.

The CLI's single source is `omafiles.usage.kdl`; `docs/cli.md` is
generated from it by `mise run usage:gen`, and `mise run check` fails
when the page is stale.

Qt 6, qmake, the C++ compiler, libarchive, GLib/GIO and KF6WindowSystem
come from the system and cannot be pinned through mise; `mise run deps`
checks that they are present. Every other tool is pinned in
`mise.toml`. Builds go to `build/` and `build-tests/`, never into the
source tree.

The full verification is `mise run check`; the versioned git hook in
`.githooks/` runs `scripts/check.sh`. The narrowest tests are one
Python suite (`python3 tests/<name>.py`) or named Qt test functions
run through the disposable environment:
`python3 scripts/test-environment.py build-tests/studio/tst_studio <function>`.
Mutations run against those. `scripts/test-environment.py` gives every
check a fresh `HOME`, `XDG_*`, `TMPDIR` and a private D-Bus session, so
no check reads or writes the user's index, Trash, mime defaults or
portal configuration. The pictures are drawn offscreen by
`mise run shots` and compared by the gate. No check moves the pointer
or types on the user's desktop; a manual run in the real session uses
the same disposable directories and a private bus, and checks window
focus before every key.

`scripts/bench` measures omafiles alone in Docker containers with
pinned CPUs and memory; it never measures or compares another
application. Its method and results are in `docs/benchmark.md` and
`docs/benchmark-results.md`.

Releases come from release-please: merge pull requests into `master` with a
merge commit, and the release workflow attaches the package `pkgbuild/PKGBUILD`
builds and its `SHA256SUMS`, which `install.sh` installs. The pre-push hook runs
`scripts/local-check`, which posts the `local-check` status `master` requires.
`docs/design.md` §7 has the whole flow.

Guides: `docs/README.md` maps every page; `docs/design.md` is for
contributors. Checks and how to run them: `docs/design.md` §9.
