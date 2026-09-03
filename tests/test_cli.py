#!/usr/bin/env python3
"""The CLI contract, end to end, against a real index in a temporary XDG home.

Every verb that is built has a case here. A verb that parses and does nothing is
indistinguishable from one that works until somebody depends on it."""

import json
import os
import pty
import re
import select
import shutil
import time
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CLI = Path(os.environ.get("OMAFILES_CLI", ROOT / "build/bin/omafiles"))


def run(data_home, *args, extra_env=None, stdin=None, cwd=None):
    env = {**os.environ, "XDG_DATA_HOME": str(data_home)}
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        [str(CLI), *map(str, args)],
        # ROOT unless a case says otherwise. The verbs that default `[dir]` to
        # the current directory can only be checked from a directory small
        # enough to state the whole answer for.
        cwd=str(cwd or ROOT),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        # Never the terminal the suite was started from. Without this the child
        # inherits it, `confirmed()` sees a tty and blocks on readLine, and
        # because stderr is piped the prompt is invisible -- so `mise run verify`
        # from a terminal hung with no output at all. The git hooks were only
        # safe by accident: git hands pre-commit /dev/null.
        **({"stdin": subprocess.DEVNULL} if stdin is None else {"input": stdin}),
    )


def run_pty(data_home, *args, answer="y\n", extra_env=None, cwd=None):
    """The same run(), over a real terminal.

    `confirmed()` refuses off a tty rather than asking, so every case about what
    the question says -- and about what has already happened by the time it is
    asked -- can only be driven through a pty. The answer is written before the
    prompt appears, which a line-disciplined terminal holds for it; a run that
    never asks simply echoes it back."""
    env = {**os.environ, "XDG_DATA_HOME": str(data_home)}
    if extra_env:
        env.update(extra_env)
    parent, child = pty.openpty()
    process = subprocess.Popen(
        [str(CLI), *map(str, args)],
        cwd=str(cwd or ROOT), env=env,
        stdin=child, stdout=child, stderr=child,
    )
    os.close(child)
    os.write(parent, answer.encode())
    seen = b""
    while True:
        ready, _, _ = select.select([parent], [], [], 20)
        if not ready:
            break
        try:
            chunk = os.read(parent, 4096)
        except OSError:                      # the child closed its end
            break
        if not chunk:
            break
        seen += chunk
    process.wait()
    os.close(parent)
    return process.returncode, seen.decode(errors="replace")


def main():
    assert CLI.is_file(), f"missing CLI at {CLI}"
    with tempfile.TemporaryDirectory(prefix="omafiles-cli-") as directory:
        data = Path(directory) / "xdg"
        workspace = Path(directory) / "files"
        workspace.mkdir(parents=True)
        data.mkdir()
        invoice = workspace / "invoice.pdf"
        notes = workspace / "q3-notes.md"
        invoice.write_text("invoice")
        notes.write_text("notes")

        added = run(data, "tag", "add", "--tag", "work", "--tag", "finance", "--", str(invoice))
        assert added.returncode == 0, added.stderr
        listed = run(data, "tag", "ls", "--", str(invoice))
        assert listed.stdout.splitlines() == ["finance", "work"], listed.stdout
        try:
            xattrs = os.listxattr(invoice)
        except OSError:
            xattrs = []
        if "user.omafiles.tags" in xattrs:
            assert os.getxattr(invoice, "user.omafiles.tags") == b"finance,work"

        tagged_notes = run(data, "tag", "add", "--tag", "work", "--", str(notes))
        assert tagged_notes.returncode == 0, tagged_notes.stderr
        work = run(data, "ls", "--tag", "work")
        assert work.returncode == 0, work.stderr
        assert work.stdout.splitlines() == [str(invoice.resolve()), str(notes.resolve())], work.stdout

        both = run(data, "ls", "--tag", "work", "--tag", "finance")
        assert both.stdout.splitlines() == [str(invoice.resolve())]

        vocab = run(data, "tags")
        assert "finance\t1" in vocab.stdout
        assert "work\t2" in vocab.stdout

        dry = run(data, "--dry-run", "tag", "rm", "--tag", "finance", "--", str(invoice))
        assert dry.returncode == 0, dry.stderr
        still = run(data, "tag", "ls", "--", str(invoice))
        assert "finance" in still.stdout

        removed = run(data, "tag", "rm", "--tag", "finance", "--", str(invoice))
        assert removed.returncode == 0, removed.stderr
        after = run(data, "tag", "ls", "--", str(invoice))
        assert after.stdout.splitlines() == ["work"], after.stdout

        json_ls = run(data, "--json", "ls", "--tag", "work")
        rows = json.loads(json_ls.stdout)
        assert [row["path"] for row in rows] == [str(invoice.resolve()), str(notes.resolve())]

        leftover = workspace / "stale.txt"
        leftover.write_text("x")
        tagged_stale = run(data, "tag", "add", "--tag", "work", "--", str(leftover))
        assert tagged_stale.returncode == 0, tagged_stale.stderr
        leftover.unlink()
        pruned = run(data, "--yes", "prune", "--all")
        assert pruned.returncode == 0, pruned.stderr
        assert str(leftover.resolve()) in pruned.stdout.splitlines()
        after_prune = run(data, "ls", "--tag", "work")
        assert str(leftover.resolve()) not in after_prune.stdout.splitlines()

        opened = run(data, "--dry-run", "open", str(notes))
        assert opened.returncode == 0, opened.stderr
        assert str(notes.resolve()) in opened.stdout
        missing_open = run(data, "open", str(workspace / "nope"))
        assert missing_open.returncode == 2

        missing = run(data, "tag", "ls", "--", str(workspace / "nope"))
        assert missing.returncode == 2

        usage = run(data)
        assert usage.returncode == 1

        # Every verb the declaration promises is built, so "not implemented" is
        # not an answer this binary has left to give. A word nobody declared is
        # a typo or a dropped feature, and says so.
        dropped = run(data, "watch", str(workspace))
        assert dropped.returncode == 1 and "unknown command" in dropped.stderr
        assert "not implemented" not in dropped.stderr

        json_env = run(data, "ls", "--tag", "work", extra_env={"OMAFILES_JSON": "1"})
        assert json.loads(json_env.stdout)

        check_tag_verbs(data, workspace)
        check_queries(data, workspace)
        check_moving(data, workspace)
        check_truth(data, workspace)
        check_tag_cp_adds(data, workspace)
        check_has_and_help_answer(data, workspace)
        check_batch(data, workspace)
        check_recent(data, workspace)
        check_completion(data, workspace)
        check_trash(data, workspace)
        check_query_flags(data, workspace)
        check_a_failed_write_is_not_a_success(data, workspace)
        check_globals(data, workspace)
        check_prune_scope(data, workspace)
        check_moving_a_folder(data, workspace)
        check_contradictory_flags(data, workspace)
        check_json_on_file_verbs(data, workspace)
        check_json_does_not_invent_a_row(data, workspace)
        check_json_array_verbs_do_not_invent_a_row(data, workspace)
        check_concurrent_writers(data, workspace)
        check_verify_walks_the_disk(data, workspace)
        check_the_disk_is_never_ahead_of_the_index(data, workspace)
        check_prune_asks(data, workspace)
        check_verify_answers_one_question(data, workspace)
        check_a_write_that_changes_nothing(data, workspace)
        check_import_validates(data, workspace)
        check_import_canonicalises(data, workspace)
        check_a_file_moved_behind_our_back(data, workspace)
        check_recent_counts_the_query(data, workspace)
        check_one_version(data, workspace)
        check_help_and_messages(data, workspace)
        check_a_link_is_not_its_target(data, workspace)
        check_a_symlink_destination_is_occupied(data, workspace)
        check_the_prompt_names_the_paths(data, workspace)
        check_mv_and_cp_answer_in_one_shape(data, workspace)
        check_prune_keeps_what_it_cannot_see(data, workspace)
        check_prune_keeps_a_file_the_owner_can_still_stat(data, workspace)
        check_verify_reports_one_cause_once(data, workspace)
        check_reindex_settles_or_says_what_will(data, workspace)
        check_reindex_quiet_keeps_the_answer(data, workspace)
        check_each_tag_subcommand_takes_its_own_flags(data, workspace)
        check_mv_carries_every_row_it_can(data, workspace)
        check_an_option_value_is_not_a_global(data, workspace)
        check_an_undated_row_is_not_stamped_by_nothing(data, workspace)
        check_a_refused_xattr_is_not_a_success(data, workspace)
        check_a_dry_run_refuses_what_the_real_run_refuses(data, workspace)
        check_from_xattr_says_when_it_declines(data, workspace)
        check_a_shut_directory_is_not_an_empty_index(data, workspace)
        check_from_xattr_writes_nothing_it_just_read(data, workspace)
        check_a_batch_prints_nothing_until_it_commits(data, workspace)
        check_a_precondition_is_settled_before_the_prompt(data, workspace)
        check_every_operand_is_tried(data, workspace)
        check_a_dry_run_needs_somewhere_to_land(data, workspace)
        check_open_is_all_or_nothing(data, workspace)
        check_a_source_it_cannot_read_through_is_missing(data, workspace)
        check_a_dir_it_may_not_look_into_is_not_a_dir_that_is_gone(data, workspace)
        check_a_scope_nobody_may_open_is_not_an_empty_answer(data, workspace)
        check_a_missing_path_is_settled_before_the_index(data, workspace)
        check_quiet_takes_the_note_beside_a_success(data, workspace)
        check_an_incomplete_answer_is_not_a_success(data, workspace)
        check_a_usage_line_carries_the_usage(data, workspace)
        check_the_divergences_are_written_down(data, workspace)
        check_a_set_is_removed_whole_or_not_at_all(data, workspace)
        check_a_link_crosses_a_filesystem_as_a_link(data, workspace)
        check_every_operand_it_refuses_is_named(data, workspace)
        check_reindex_names_the_path_it_walked_away_from(data, workspace)
        check_tag_clear_does_not_claim_a_drop_it_did_not_make(data, workspace)
        check_tag_rm_does_not_claim_a_drop_it_did_not_make(data, workspace)
        check_completion_declines_a_flag_the_verb_refuses(data, workspace)
        check_a_no_op_clear_invents_no_date(data, workspace)
        check_a_path_it_may_not_look_at_is_not_a_path_that_is_gone(data, workspace)
        check_a_batch_carries_a_line_that_did_not_settle(data, workspace)
        check_import_reads_either_shape_export_writes(data, workspace)
        check_a_batch_line_is_not_a_shell_line(data, workspace)
        check_verify_says_only_what_it_did(data, workspace)
        check_a_preview_runs_where_a_read_runs(data, workspace)
        check_a_message_names_what_the_verb_takes(data, workspace)
        check_a_name_the_walk_cannot_spell_is_reported(data, workspace)
        check_the_json_shapes_are_written_down(data, workspace)
        check_a_move_rewrites_only_the_mirror_it_left_behind(data, workspace)
        check_a_batch_reports_the_rows_it_kept(data, workspace)
        check_an_import_reports_one_row_per_path(data, workspace)
        check_a_verify_detail_stays_one_field(data, workspace)
        check_batch_json_prints_nothing_on_rollback(data, workspace)
        check_has_on_a_path_nobody_may_look_at(data, workspace)

    print("ok")


def check_tag_verbs(data, workspace):
    """tag clear, tag cp, tag merge."""
    source = workspace / "source.txt"
    target = workspace / "target.txt"
    source.write_text("s")
    target.write_text("t")

    run(data, "tag", "add", "--tag", "alpha", "--tag", "beta", "--", str(source))

    copied = run(data, "tag", "cp", "--from", str(source), "--", str(target))
    assert copied.returncode == 0, copied.stderr
    assert run(data, "tag", "ls", "--", str(target)).stdout.split() == ["alpha", "beta"]

    merged = run(data, "tag", "merge", "alpha", "gamma")
    assert merged.returncode == 0, merged.stderr
    assert run(data, "tag", "ls", "--", str(source)).stdout.split() == ["beta", "gamma"]
    assert "alpha" not in run(data, "tags").stdout

    cleared = run(data, "tag", "clear", "--", str(target))
    assert cleared.returncode == 0, cleared.stderr
    assert run(data, "tag", "ls", "--", str(target)).stdout.strip() == ""
    # Clearing takes the xattr with it, or the file would still claim the tags.
    assert "user.omafiles.tags" not in os.listxattr(target)

    unknown = run(data, "tag", "nonsense")
    assert unknown.returncode == 1
    assert "unknown subcommand" in unknown.stderr


def check_queries(data, workspace):
    """has, json, export, import."""
    subject = workspace / "subject.txt"
    subject.write_text("x")
    run(data, "tag", "add", "--tag", "hunted", "--", str(subject))

    assert run(data, "has", "--tag", "hunted", "--", str(subject)).returncode == 0
    # Exit 1 for "no", the way grep -q does it.
    assert run(data, "has", "--tag", "absent", "--", str(subject)).returncode == 1

    row = json.loads(run(data, "json", "--", str(subject)).stdout)
    assert row["path"] == str(subject.resolve())
    assert "hunted" in row["tags"]

    dumped = run(data, "export")
    assert dumped.returncode == 0, dumped.stderr
    rows = [json.loads(line) for line in dumped.stdout.splitlines() if line]
    assert str(subject.resolve()) in [row["path"] for row in rows]

    # An import merges: it adds what the dump knows without dropping what the
    # index knows that the dump does not.
    dump = workspace / "dump.jsonl"
    dump.write_text(json.dumps({
        "path": str(subject.resolve()),
        "tags": ["imported"],
    }) + "\n")
    imported = run(data, "import", str(dump))
    assert imported.returncode == 0, imported.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["hunted", "imported"]

    # Bad input is exit 1, not 3. Three is what a caller reads as "the index
    # could not be written", and a dump the user typed wrong is not that -- the
    # two used to be the same answer from the same verb.
    broken = workspace / "broken.jsonl"
    broken.write_text("not json\n")
    torn = run(data, "import", str(broken))
    assert torn.returncode == 1, torn.returncode
    assert "line 1" in torn.stderr, torn.stderr

    # And the shapes that used to be waved through. Each names what is wrong
    # with it rather than the first thing that went missing downstream.
    for row, expected in (
        ('{"path":123,"tags":["x"]}', "non-string path"),
        ('{"tags":["x"]}', "missing path"),
        ('{"path":"/tmp/x","tags":"notalist"}', "non-list tags"),
        ('{"path":"relative/path","tags":["x"]}', "absolute path"),
    ):
        broken.write_text(row + "\n")
        said = run(data, "import", str(broken))
        assert said.returncode == 1, (row, said.returncode)
        assert expected in said.stderr, (row, said.stderr)
        assert "relative/path" not in run(data, "export").stdout

    # A row for a file that is simply not on this machine is still importable:
    # merging a dump from somewhere else is a real thing to do.
    broken.write_text('{"path":"/nowhere/at/all/x.txt","tags":["elsewhere"]}\n')
    assert run(data, "import", str(broken)).returncode == 0
    assert "/nowhere/at/all/x.txt" in run(data, "export").stdout

    # A merge adds; it does not un-know things. A dump line with no `tagged`
    # field used to overwrite the stamp on the row it merged into, so `--since`
    # could never judge that file again and `ls --since` started calling a file
    # tagged seconds ago older than the field itself.
    stamped_row = json.loads(run(data, "--json", "json", "--", str(subject)).stdout)
    assert stamped_row["tagged"] > 0
    broken.write_text(json.dumps({"path": str(subject.resolve())}) + "\n")
    assert run(data, "import", str(broken)).returncode == 0
    kept = json.loads(run(data, "--json", "json", "--", str(subject)).stdout)
    assert kept["tagged"] == stamped_row["tagged"], "import erased the tagging stamp"
    assert kept["tags"] == stamped_row["tags"], "import dropped tags it was merging into"


def check_moving(data, workspace):
    """mv, cp, rm -- the index has to follow the file."""
    original = workspace / "movable.txt"
    original.write_text("m")
    run(data, "tag", "add", "--tag", "travels", "--", str(original))

    moved_to = workspace / "moved.txt"
    moved = run(data, "mv", str(original), str(moved_to))
    assert moved.returncode == 0, moved.stderr
    assert moved_to.exists() and not original.exists()
    assert run(data, "tag", "ls", "--", str(moved_to)).stdout.split() == ["travels"]
    assert str(original.resolve()) not in run(data, "export").stdout

    copy_to = workspace / "copied.txt"
    copied = run(data, "cp", str(moved_to), str(copy_to))
    assert copied.returncode == 0, copied.stderr
    assert copy_to.exists() and moved_to.exists()
    assert run(data, "tag", "ls", "--", str(copy_to)).stdout.split() == ["travels"]

    # Neither overwrites: the index cannot describe a file that was replaced
    # behind its back.
    assert run(data, "cp", str(moved_to), str(copy_to)).returncode == 1
    assert run(data, "mv", str(moved_to), str(copy_to)).returncode == 1

    # A directory in the list is a refusal `rm` can make by looking, so it is
    # made before anything is unlinked -- see
    # check_a_precondition_is_settled_before_the_prompt. Nothing goes, and no
    # row goes with it.
    partial = workspace / "partial"
    partial.mkdir()
    first = partial / "first.txt"
    blocker = partial / "blocker"          # a directory: rm refuses it
    last = partial / "last.txt"
    first.write_text("1")
    blocker.mkdir()
    last.write_text("2")
    run(data, "tag", "add", "--tag", "partial", "--", str(first), str(last))

    stopped = run(data, "--yes", "rm", "--", str(first), str(blocker), str(last))
    assert stopped.returncode == 1
    assert "is a directory" in stopped.stderr
    assert first.exists(), "a path was removed for a run that was going to refuse"
    assert last.exists(), "a path was removed for a run that was going to refuse"

    rows = run(data, "export").stdout
    assert str(first.resolve()) in rows and str(last.resolve()) in rows

    # And with the blocker out of the list, all of it goes -- rows included.
    blocker.rmdir()
    swept = run(data, "--yes", "rm", "--", str(first), str(last))
    assert swept.returncode == 0, swept.stderr
    rows = run(data, "export").stdout
    assert str(first.resolve()) not in rows and str(last.resolve()) not in rows


    kept = run(data, "rm", "--", str(copy_to))
    assert kept.returncode == 1 and copy_to.exists()
    assert "--yes" in kept.stderr

    gone = run(data, "--yes", "rm", "--", str(copy_to))
    assert gone.returncode == 0, gone.stderr
    assert not copy_to.exists()
    assert str(copy_to.resolve()) not in run(data, "export").stdout


def check_truth(data, workspace):
    """reindex and verify -- the index against what the files themselves say."""
    subject = workspace / "truth.txt"
    subject.write_text("t")
    run(data, "tag", "add", "--tag", "indexed", "--", str(subject))

    clean = run(data, "verify", str(workspace))
    assert clean.returncode == 0, clean.stdout + clean.stderr

    os.setxattr(subject, "user.omafiles.tags", b"ondisk")
    drifted = run(data, "verify", str(workspace))
    assert drifted.returncode == 1
    assert "xattr-differs" in drifted.stdout

    # Without the flag the index keeps its answer -- and makes that true on the
    # disk, so the disagreement is gone. It used to keep its answer and stop
    # there: `verify` reported, `reindex` said nothing and changed nothing, and
    # `verify` reported the same thing again. The default direction of this verb
    # was the one problem the program could see and no verb could settle.
    settled = run(data, "reindex", str(workspace))
    assert settled.returncode == 0, settled.stderr
    assert str(subject.resolve()) in settled.stdout, "reindex changed a row and said nothing"
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["indexed"]
    assert os.getxattr(subject, "user.omafiles.tags").decode() == "indexed"
    assert run(data, "verify", str(workspace)).returncode == 0, "reindex left them apart"

    # With the flag, the file's own tags win instead.
    os.setxattr(subject, "user.omafiles.tags", b"ondisk")
    reindexed = run(data, "reindex", "--from-xattr", str(workspace))
    assert reindexed.returncode == 0, reindexed.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["ondisk"]
    assert run(data, "verify", str(workspace)).returncode == 0

    # A file the index never knew about is adopted from its xattr.
    stray = workspace / "stray.txt"
    stray.write_text("s")
    os.setxattr(stray, "user.omafiles.tags", b"found")
    run(data, "reindex", str(workspace))
    assert run(data, "tag", "ls", "--", str(stray)).stdout.split() == ["found"]


def check_tag_cp_adds(data, workspace):
    """`tag cp` adds tags to the destination; it never takes any away.

    It used to write the source's tag set over the destination's, so copying
    from a file with no tags erased every tag on the target and dropped its row
    -- no output, no warning, exit 0."""
    room = workspace / "copying"
    room.mkdir()
    source = room / "src.md"
    destination = room / "dst.md"
    source.write_text("s")
    destination.write_text("d")
    run(data, "tag", "add", "--tag", "important", "--tag", "keep", "--", str(destination))

    # Nothing to copy is a mistake, not an instruction to erase.
    empty = run(data, "tag", "cp", "--from", str(source), "--", str(destination))
    assert empty.returncode == 1, empty.returncode
    assert "no tags to copy" in empty.stderr, empty.stderr
    assert run(data, "tag", "ls", "--", str(destination)).stdout.split() == ["important", "keep"]

    # And a real copy adds rather than replaces.
    run(data, "tag", "add", "--tag", "beta", "--", str(source))
    added = run(data, "tag", "cp", "--from", str(source), "--", str(destination))
    assert added.returncode == 0, added.stderr
    assert run(data, "tag", "ls", "--", str(destination)).stdout.split() \
        == ["beta", "important", "keep"]
    assert os.getxattr(destination, "user.omafiles.tags").decode() == "beta,important,keep"


def check_has_and_help_answer(data, workspace):
    """`has --json` answers, and `--help` answers about the verb you named."""
    room = workspace / "answering"
    room.mkdir()
    subject = room / "h.txt"
    subject.write_text("h")
    run(data, "tag", "add", "--tag", "present", "--", str(subject))

    # The exit code was the whole answer, so `--json has` emitted no bytes at
    # all and a JSON consumer got an empty stream.
    for tag, expected, code in (("present", True, 0), ("absent", False, 1)):
        spoken = run(data, "--json", "has", "--tag", tag, "--", str(subject))
        assert spoken.returncode == code, (tag, spoken.returncode)
        answer = json.loads(spoken.stdout)
        assert answer["has"] is expected and answer["tag"] == tag
        assert answer["path"] == str(subject.resolve())

    # `omafiles ls --help` used to print the whole screen, which is the one
    # thing somebody asking about `ls` already had.
    for verb, wanted, unwanted in (("ls", "--untagged", "completions"),
                                   ("prune", "--all", "--untagged"),
                                   ("batch", "one command per line", "--untagged")):
        page = run(data, verb, "--help")
        assert page.returncode == 0, page.stderr
        assert page.stdout.startswith(f"Usage: omafiles {verb}"), page.stdout
        assert wanted in page.stdout, page.stdout
        assert unwanted not in page.stdout, page.stdout

    # `tag add` is two words, and `tag` alone still names all six.
    page = run(data, "tag", "add", "--help")
    assert page.stdout.startswith("Usage: omafiles tag add"), page.stdout
    assert "tag clear" not in page.stdout
    assert "tag clear" in run(data, "tag", "--help").stdout

    # An alias is a spelling of the command, including when it asks for help.
    # `printHelp` matched the body's literal entry text, which only spells `tag
    # merge`, so `tag rename --help` failed -- for a word `dispatch` accepts and
    # docs/cli.md lists under Aliases.
    page = run(data, "tag", "rename", "--help")
    assert page.returncode == 0, page.stderr
    assert "tag merge" in page.stdout, page.stdout
    assert "tag clear" not in page.stdout, page.stdout

    # And a miss names what was actually asked about. Reporting only the first
    # word answered `tag rename --help` with `unknown command: tag`, which is a
    # command that exists.
    missed = run(data, "tag", "nonsense", "--help")
    assert missed.returncode == 1
    assert "unknown command: tag nonsense" in missed.stderr, missed.stderr

    # A word that is not a verb is a typo, and `--help` after it does not make it
    # one. Printing the whole screen and exiting 0 was the program agreeing with
    # the typo: `omafiles lsit --help` read as an answer about `lsit`, while the
    # same word without `--help` had always been an error. Both spellings say the
    # same thing now, and it goes to stderr with nothing on stdout -- a reader who
    # piped the page into a pager would otherwise get a page about everything.
    stray = run(data, "nonsense", "--help")
    assert stray.returncode == 1, stray.returncode
    assert stray.stdout == "", stray.stdout
    assert "unknown command" in stray.stderr, stray.stderr
    assert "nonsense" in stray.stderr, stray.stderr

    # And the whole screen is still reachable, by the one spelling that asks for
    # it: `completions` is only ever on the full page.
    whole = run(data, "--help")
    assert whole.returncode == 0, whole.stderr
    assert "completions" in whole.stdout, whole.stdout

    # `mv` and `cp` honour --json in a dry run like every other verb.
    for verb in ("mv", "cp"):
        planned = run(data, "--json", "--dry-run", verb,
                      str(subject), str(room / f"moved-{verb}.txt"))
        assert planned.returncode == 0, planned.stderr
        assert json.loads(planned.stdout)["from"] == str(subject.resolve())

    # Completion knows that `tag merge` takes a tag, not a filename.
    offered = run(data, "__complete", "--", "tag", "merge", "")
    assert offered.returncode == 0, offered.returncode
    assert "present" in offered.stdout.split(), offered.stdout


def check_batch(data, workspace):
    """batch -- many commands over one index, all of them or none."""
    room = workspace / "batch"
    room.mkdir()
    many = [room / f"file{n}.txt" for n in range(4)]
    for path in many:
        path.write_text("x")
    spaced = room / "two words.txt"
    spaced.write_text("x")

    script = room / "commands.txt"
    script.write_text(
        "# a comment, and the blank line under it, are skipped\n"
        "\n"
        + "".join(f"tag add --tag many -- {path}\n" for path in many)
        # Quoted, because a path with a space in it is one argument.
        + f'tag add --tag spaced -- "{spaced}"\n'
    )

    ran = run(data, "batch", str(script))
    assert ran.returncode == 0, ran.stderr
    for path in many:
        assert run(data, "tag", "ls", "--", str(path)).stdout.split() == ["many"]
        # And the file's own copy. Only the rollback case was asserted, which is
        # the *absence* of an xattr -- trivially true if a batch never writes
        # one at all, so deleting the flush at the end of `cmdBatch` left the
        # whole suite green while every batched file went unstamped.
        assert os.getxattr(path, "user.omafiles.tags") == b"many", path
    assert run(data, "tag", "ls", "--", str(spaced)).stdout.split() == ["spaced"]
    assert run(data, "verify", str(room)).returncode == 0, "a batch left index and disk apart"

    # Every line of one batch, in order, against one index: the tag the second
    # line adds is on the row the first line wrote, not on a row that was reread
    # from disk in between.
    subject = room / "layered.txt"
    subject.write_text("x")
    layered = run(data, "batch", "-", stdin=(
        f"tag add --tag first -- {subject}\n"
        f"tag add --tag second -- {subject}\n"
        f"tag rm --tag first -- {subject}\n"
    ))
    assert layered.returncode == 0, layered.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["second"]

    # A line that fails takes the whole run with it, and exits with that line's
    # own code: a half-applied batch would be worse than no batch at all.
    doomed = room / "doomed.txt"
    doomed.write_text("x")
    failing = run(data, "batch", "-", stdin=(
        f"tag add --tag rolled -- {doomed}\n"
        f"tag add --tag rolled -- {room / 'not-here.txt'}\n"
    ))
    assert failing.returncode == 2, failing.stderr
    assert "line 2" in failing.stderr and "not-here.txt" in failing.stderr
    assert run(data, "tag", "ls", "--", str(doomed)).stdout.strip() == ""
    # Not even the file's own copy of the tags survives the rollback.
    assert "user.omafiles.tags" not in os.listxattr(doomed)

    dry = run(data, "--dry-run", "batch", "-",
              stdin=f"tag add --tag never -- {many[0]}\n")
    assert dry.returncode == 0, dry.stderr
    assert str(many[0].resolve()) in dry.stdout.splitlines()
    assert "never" not in run(data, "tag", "ls", "--", str(many[0])).stdout

    rows = json.loads(run(data, "--json", "batch", "-", stdin=(
        f"tag add --tag reported -- {many[1]}\n"
        f"tag add --tag reported -- {many[2]}\n"
    )).stdout)
    assert [row["path"] for row in rows] == [str(many[1].resolve()), str(many[2].resolve())]

    # The verbs a batch cannot promise anything about are named and refused,
    # never quietly skipped.
    moved_to = room / "moved.txt"
    refused = run(data, "batch", "-", stdin=f"mv {many[3]} {moved_to}\n")
    assert refused.returncode == 1 and "mv" in refused.stderr
    assert many[3].exists() and not moved_to.exists()

    read = run(data, "batch", "-", stdin="ls --tag many\n")
    assert read.returncode == 1 and "ls" in read.stderr

    nested = run(data, "batch", "-", stdin=f"batch {script}\n")
    assert nested.returncode == 1 and "nest" in nested.stderr

    missing_file = run(data, "batch", str(room / "no-such-file"))
    assert missing_file.returncode == 2




def check_recent(data, workspace):
    """recent, and the two rules that decide what it means."""
    first = workspace / "first.md"
    second = workspace / "second.md"
    first.write_text("1")
    second.write_text("2")

    run(data, "tag", "add", "--tag", "ordered", "--", str(first))
    time.sleep(1.1)
    run(data, "tag", "add", "--tag", "ordered", "--", str(second))

    listing = run(data, "recent")
    assert listing.returncode == 0, listing.stderr
    order = [line for line in listing.stdout.splitlines()
             if line in (str(first.resolve()), str(second.resolve()))]
    assert order == [str(second.resolve()), str(first.resolve())], listing.stdout

    assert len(run(data, "recent", "1").stdout.splitlines()) == 1
    assert run(data, "recent", "0").returncode == 1
    assert run(data, "recent", "many").returncode == 1

    stamp = lambda path: json.loads(run(data, "--json", "json", "--", str(path)).stdout)["tagged"]

    # Moving a file is not tagging it, so the stamp comes across unchanged.
    moved = workspace / "moved-stamp.md"
    before = stamp(first)
    run(data, "mv", str(first), str(moved))
    assert stamp(moved) == before

    # A copy was tagged now, so it gets its own.
    time.sleep(1.1)
    copy = workspace / "copied-stamp.md"
    run(data, "cp", str(moved), str(copy))
    assert stamp(copy) > before

    # --since counts from when it was tagged, not when the file was touched.
    aged = workspace / "aged.md"
    aged.write_text("old")
    run(data, "tag", "add", "--tag", "aged", "--", str(aged))
    os.utime(aged, (0, 0))
    assert str(aged.resolve()) in run(data, "ls", "--tag", "aged", "--since", "1h").stdout


def check_completion(data, workspace):
    """__complete, which is where the shell scripts get their answers."""
    subject = workspace / "completable.md"
    subject.write_text("c")
    run(data, "tag", "add", "--tag", "zeta", "--", str(subject))

    verbs = run(data, "__complete", "--", "").stdout.split()
    assert "tag" in verbs and "recent" in verbs and "batch" in verbs

    subs = run(data, "__complete", "--", "tag", "").stdout.split()
    assert subs[:3] == ["add", "rm", "ls"]

    # The one completion no generated script could produce: your own tags.
    tags = run(data, "__complete", "--", "ls", "--tag", "").stdout.split()
    assert "zeta" in tags

    flags = run(data, "__complete", "--", "ls", "--").stdout.split()
    assert "--untagged" in flags and "--since" in flags

    # A word that starts with `-` asks for flags, even where a subcommand or a
    # shell name would otherwise be offered. `tag -` answered with the six
    # subcommand names, `completions -` with bash, zsh and fish, and `tag merge
    # -` with the tag vocabulary -- none of which start with `-`. Under the
    # emitted bash script the reply then filtered down to nothing at all, and
    # exiting 0 stopped the shell falling back to filenames as well, so the tab
    # did nothing whatsoever. Every other verb already branched on `-` first,
    # as cargo, clap and cobra all do.
    for words in (["tag"], ["completions"], ["tag", "merge"]):
        dashed = run(data, "__complete", "--", *words, "-")
        assert dashed.returncode == 0, dashed.stderr
        offered = dashed.stdout.split()
        assert offered, f"{words} offered nothing for a bare -"
        assert all(word.startswith("-") for word in offered), (words, offered)
        assert "--quiet" in offered, (words, offered)
        # Every verb but the one that refuses --json. The offer was made
        # anyway, so the shell suggested the flag and the verb -- once it
        # started refusing it -- would have turned it down: completion for an
        # error message.
        assert ("--json" in offered) == (words != ["completions"]), (words, offered)

    # And what is offered is what the subcommand will take. Completion
    # advertised --tag and --from for the whole `tag` family, so it suggested
    # `tag clear --tag` -- the exact command that used to clear every tag on the
    # path, and that the per-subcommand gates now refuse. Asked of the gate
    # itself rather than a list written out here, so the two cannot drift.
    for sub in ("add", "rm", "ls", "clear", "cp", "merge", "rename"):
        offered = run(data, "__complete", "--", "tag", sub, "-").stdout.split()
        assert "--json" in offered, (sub, offered)
        for flag in ("--tag", "--from"):
            # --dry-run, because the spellings the gate does accept would
            # otherwise write the tag `value` into the index behind this check.
            gated = run(data, "-n", "tag", sub, flag, "value", "--", str(subject))
            refused = f"unexpected {flag}" in gated.stderr
            assert (flag in offered) != refused, (sub, flag, offered, gated.stderr)

    # No opinion about a path: exit 1 tells the stub to let the shell try files.
    assert run(data, "__complete", "--", "tag", "add", "--tag", "zeta", "--", "").returncode == 1

    for shell in ("bash", "zsh", "fish"):
        script = run(data, "completions", shell)
        assert script.returncode == 0, script.stderr
        assert "__complete" in script.stdout
    assert run(data, "completions", "tcsh").returncode == 1
    assert run(data, "completions").returncode == 1

    # A shell script has no JSON shape, which is what the README says --  but
    # the flag was taken and ignored, so the only place the exception was
    # written down was the one place nobody runs. A global that one verb does
    # not have has to be refused by that verb, or it is not an exception.
    refused = run(data, "completions", "--json", "bash")
    assert refused.returncode == 1, refused.stdout
    assert "--json" in refused.stderr, refused.stderr
    assert refused.stdout.strip() == "", refused.stdout
    # An OMAFILES_JSON in the environment is a default and not a request.
    # Refusing that too would break `source <(omafiles completions bash)` in
    # every shell that exports one.
    defaulted = run(data, "completions", "bash", extra_env={"OMAFILES_JSON": "1"})
    assert defaulted.returncode == 0, defaulted.stderr
    assert "__complete" in defaulted.stdout


def check_trash(data, workspace):
    """trash -- the one verb that hands the file to somebody else."""
    if shutil.which("gio") is None:
        return

    # The home trash is $XDG_DATA_HOME/Trash, which the harness already points
    # inside the temporary tree -- so gio lands on the same filesystem as the
    # file and nowhere near the real desktop's wastebasket. HOME is redirected
    # too, so nothing falls back to it.
    #
    # It must NOT override XDG_DATA_HOME: that is where the index lives, and
    # pointing it elsewhere made every `export` here read an empty index, so the
    # assertions passed without measuring anything.
    env = {"HOME": str(workspace.parent / "home")}
    (workspace.parent / "home").mkdir(exist_ok=True)

    doomed = workspace / "doomed.md"
    doomed.write_text("d")
    tagged = run(data, "tag", "add", "--tag", "doomed", "--", str(doomed), extra_env=env)
    assert tagged.returncode == 0, tagged.stderr

    # Destructive, so off a terminal it refuses rather than guessing.
    kept = run(data, "trash", "--", str(doomed), extra_env=env)
    assert kept.returncode == 1 and doomed.exists()
    assert "--yes" in kept.stderr

    trashed = run(data, "--yes", "trash", "--", str(doomed), extra_env=env)
    if trashed.returncode != 0:
        # No trash directory reachable from here. The row must survive, because
        # a file that is still on disk with no tags is worse than a failure.
        assert doomed.exists(), trashed.stderr
        assert str(doomed.resolve()) in run(data, "export", extra_env=env).stdout
        return

    assert not doomed.exists()
    assert (data / "Trash/files/doomed.md").exists()
    left = run(data, "export", extra_env=env).stdout
    assert str(doomed.resolve()) not in left
    # And the index it read is the one with the other rows in it, or the line
    # above would be true of any empty file.
    assert "\"tags\"" in left


def check_query_flags(data, workspace):
    """The ls filters. Each of these survived being inverted before it had a case.

    The tag names are its own: the checks before this one add, merge and clear
    tags of their own, and a filter test that shares a name with them is a test
    measuring somebody else's rows."""
    room = workspace / "flags"
    room.mkdir()
    ods = room / "sheet.ods"
    txt = room / "notes.txt"
    bare = room / "untagged.txt"
    for f in (ods, txt, bare):
        f.write_text("x")
    run(data, "tag", "add", "--tag", "flag-a", "--", str(ods))
    run(data, "tag", "add", "--tag", "flag-b", "--", str(txt))

    # --any is OR. As AND it would return nothing, since no file has both.
    either = run(data, "ls", "--any", "flag-a", "--any", "flag-b").stdout.split("\n")
    assert str(ods.resolve()) in either and str(txt.resolve()) in either

    # --tag is AND, and the same two tags that matched either match neither.
    assert run(data, "ls", "--tag", "flag-a", "--tag", "flag-b").stdout.strip() == ""

    # --ext filters, and normalises a leading dot and case.
    only_ods = run(data, "ls", "--any", "flag-a", "--any", "flag-b", "--ext", "ods")
    assert only_ods.stdout.splitlines() == [str(ods.resolve())]
    assert run(data, "ls", "--ext", ".ODS", "--any", "flag-a").stdout.splitlines() \
        == [str(ods.resolve())]

    # --untagged lists what has no tags in the directory it is given, and reads
    # an absent one as the current directory the way verify, reindex and prune
    # all do. It used to exit 1 with `needs a directory`, which the declaration
    # -- `arg "[dir]"`, optional -- never said it would.
    here = run(data, "ls", "--untagged", cwd=room)
    assert here.returncode == 0, here.stderr
    assert str(bare.resolve()) in here.stdout.splitlines(), here.stdout
    untagged = run(data, "ls", "--untagged", str(room)).stdout.splitlines()
    assert str(bare.resolve()) in untagged
    assert str(ods.resolve()) not in untagged

    # The same entries verify and reindex walk: a dotfile and a directory are
    # both things somebody can tag, so both are things that can be untagged.
    # Only a plain visible file was ever asserted, so reverting the walk to
    # `QDir::Files` left this green.
    hidden = room / ".hidden.txt"
    hidden.write_text("h")
    folder = room / "plainfolder"
    folder.mkdir()
    untagged = run(data, "ls", "--untagged", str(room)).stdout.splitlines()
    assert str(hidden.resolve()) in untagged, "--untagged cannot see a dotfile"
    assert str(folder.resolve()) in untagged, "--untagged cannot see a directory"

    # And it stays inside the tree it was given. Losing NoDotAndDotDot sends the
    # walk up through `..` and out over the whole filesystem.
    assert all(line.startswith(str(room.resolve()) + "/") for line in untagged), untagged
    assert str(room.resolve()) not in untagged, "--untagged listed the folder it was asked about"

    # A symlink and its target are one entry, listed once. The walk dedups on
    # the canonical path, and `verify` and `reindex` both have their own reasons
    # not to notice if it stops -- so this is the only place that can say.
    twinned = room / "twinned"
    twinned.mkdir()
    original = twinned / "once.txt"
    original.write_text("o")
    os.symlink("once.txt", twinned / "twice.txt")
    listed = run(data, "ls", "--untagged", str(twinned)).stdout.splitlines()
    assert listed == [str(original.resolve())], listed

    # The contradictions are reported before the missing directory, and each
    # names the flag that was actually typed. Both used to say `--tag`, and the
    # missing-directory check ran first -- so somebody who typed `--untagged
    # --any work` was told to add a directory, and only after doing as they were
    # told heard about a flag they had not used.
    for flag in ("--tag", "--any"):
        conflicted = run(data, "ls", "--untagged", flag, "work")
        assert conflicted.returncode == 1
        assert "opposite questions" in conflicted.stderr, conflicted.stderr
        assert flag in conflicted.stderr, (flag, conflicted.stderr)
        assert "needs a directory" not in conflicted.stderr, conflicted.stderr

    # A directory is not an answer to a question about file extensions, and it
    # is not one on either side of the flag either. `--ext` used to mean two
    # things: the untagged walk skipped directories and the index scan did not,
    # so a tagged folder called `notes.bak` came back from `ls --ext bak` and
    # not from `ls --untagged --ext bak`.
    suffixed = room / "archive.bak"
    suffixed.mkdir()
    by_ext = run(data, "ls", "--untagged", "--ext", "bak", str(room)).stdout.splitlines()
    assert str(suffixed.resolve()) not in by_ext, by_ext
    tagged_folder = room / "notes.bak"
    tagged_folder.mkdir()
    run(data, "tag", "add", "--tag", "flag-dir", "--", str(tagged_folder))
    from_index = run(data, "ls", "--tag", "flag-dir", "--ext", "bak").stdout.splitlines()
    assert from_index == [], from_index
    assert run(data, "ls", "--tag", "flag-dir").stdout.strip() != ""

    # A leading dot names the file, it does not introduce an extension.
    # `QFileInfo::suffix()` hands back everything after the last dot, so `--ext
    # hidden` claimed `.hidden` is a file of type `hidden`. Python's
    # `os.path.splitext('.cshrc')` and Go (golang/go#24239) both say otherwise.
    # All four shapes, because only `name.ext` was ever asserted.
    dotted = room / ".dotted"
    dotted_ext = room / ".dotted.log"
    plain = room / "plainname"
    for f in (dotted, dotted_ext, plain):
        f.write_text("d")
    run(data, "tag", "add", "--tag", "flag-dot", "--",
        str(dotted), str(dotted_ext), str(plain))
    assert run(data, "ls", "--tag", "flag-dot", "--ext", "dotted").stdout.strip() == ""
    assert run(data, "ls", "--tag", "flag-dot", "--ext", "plainname").stdout.strip() == ""
    assert run(data, "ls", "--tag", "flag-dot", "--ext", "log").stdout.splitlines() \
        == [str(dotted_ext.resolve())]

    # A directory nobody may open is not an empty directory. `verify` says so
    # and `ls --untagged` walked past it in silence, so a tree that had never
    # been looked inside came back as a tree with nothing untagged in it.
    # On stderr, where --quiet can put it, not on stdout among the answers.
    shut = room / "shut"
    shut.mkdir()
    (shut / "buried.txt").write_text("b")
    shut.chmod(0o000)
    try:
        blind = run(data, "ls", "--untagged", str(room))
        assert "unreadable" in blind.stderr, blind.stderr
        assert str(shut.resolve()) in blind.stderr, blind.stderr
        assert "unreadable" not in blind.stdout, blind.stdout
        hushed = run(data, "--quiet", "ls", "--untagged", str(room))
        assert hushed.stderr.strip() == "", hushed.stderr

        # And the directory named on the command line gets the same test the
        # ones under it get. `QDirIterator` never yields the directory it was
        # pointed at, so only the descendants were ever asked: `ls --untagged
        # shut` walked into a directory it could not open, printed nothing and
        # exited 0, which reads as "there is nothing untagged here" about a
        # tree it had never looked inside. `verify shut` answered `unreadable`
        # on that same input all along, /usr/bin/ls says `cannot open
        # directory` and /usr/bin/find `Permission denied`; none of the three
        # calls it empty.
        named = run(data, "ls", "--untagged", str(shut))
        assert "unreadable" in named.stderr, named.stderr
        assert str(shut.resolve()) in named.stderr, named.stderr
        assert named.stdout.strip() == "", named.stdout
        # Exit 1, like the descendant one line up: a status that meant
        # "unreadable" for the root and nothing for a directory inside it would
        # rebuild the asymmetry this is about, one level higher. Both are 1
        # because a list one row short is not the answer that was asked for --
        # see check_an_incomplete_answer_is_not_a_success. The note said so on
        # stderr and then exited 0, and `--quiet`, which is allowed to take the
        # note away, took the only sign with it.
        assert named.returncode == 1, named.stderr
        assert run(data, "--quiet", "ls", "--untagged", str(shut)).returncode == 1
        blind_status = run(data, "ls", "--untagged", str(room))
        assert blind_status.returncode == 1, blind_status.stderr
    finally:
        shut.chmod(0o755)

    # --since counts from the tag stamp. A row without one cannot be judged, and
    # ls says so on stderr rather than returning a short list that looks whole.
    index = data / "omafiles/index.jsonl"
    legacy = room / "legacy.txt"
    legacy.write_text("l")
    with index.open("a") as handle:
        handle.write(json.dumps({
            "path": str(legacy.resolve()), "tags": ["flag-a"],
            }) + "\n")

    recent_only = run(data, "ls", "--tag", "flag-a", "--since", "1h")
    assert str(ods.resolve()) in recent_only.stdout
    assert str(legacy.resolve()) not in recent_only.stdout
    assert "predate the tagged field" in recent_only.stderr

    # And the duration is read, not just the flag. Written with a stamp instead
    # of slept for: the comparison is in whole seconds and inclusive, so a test
    # that waits is a test that flakes on the boundary.
    aged = room / "aged.txt"
    aged.write_text("a")
    with index.open("a") as handle:
        handle.write(json.dumps({
            "path": str(aged.resolve()), "tags": ["flag-old"],
                "tagged": int(time.time()) - 600,
        }) + "\n")
    assert str(aged.resolve()) in run(data, "ls", "--tag", "flag-old", "--since", "1h").stdout
    assert run(data, "ls", "--tag", "flag-old", "--since", "1m").stdout.strip() == ""

    # A duration too big to hold is refused, not folded into one that fits.
    # `toLongLong` with no `ok` pointer returns 0, so the widest window anybody
    # could type became `0s` and matched nothing -- indistinguishable, from
    # outside, from a week in which nothing was tagged. Just below that the
    # multiplication into seconds overflowed and the error was never assigned,
    # so it exited 1 having printed a bare newline.
    for huge in ("99999999999999999999d", "999999999999999d", "9223372036854775807d"):
        refused = run(data, "ls", "--tag", "flag-a", "--since", huge)
        assert refused.returncode == 1, (huge, refused.stdout)
        assert refused.stdout.strip() == "", (huge, refused.stdout)
        assert "invalid duration" in refused.stderr, (huge, refused.stderr)
    # The largest window that does fit is still a window, not a rejection.
    assert run(data, "ls", "--tag", "flag-a", "--since", "106751991167d").returncode == 0

    # And an empty one is refused rather than dropped. It was the only spelling
    # of "absent" the flag had, so `ls --since "$WINDOW"` with WINDOW unset took
    # the filter away and listed the whole index -- while `--tag ""` and
    # `--any ""` were both errors.
    blank = run(data, "ls", "--since", "")
    assert blank.returncode == 1, blank.stdout
    assert blank.stdout.strip() == "", blank.stdout
    assert "invalid duration" in blank.stderr, blank.stderr


def check_a_failed_write_is_not_a_success(data, workspace):
    """stdout that could not be written is reported, whichever verb wrote it.

    `omafiles ls > /dev/full` truncated its answer and exited 0, so make and
    anything else that reads an exit status was told a partial list was the
    whole one. coreutils has checked this in `close_stdout` for two decades:
    the last write lands in the buffer and the *flush* is what fails, so the
    check is after the flush at exit, once, for every verb."""
    room = workspace / "fullness"
    room.mkdir()
    subject = room / "w.txt"
    subject.write_text("w")
    run(data, "tag", "add", "--tag", "written", "--", str(subject))

    full = Path("/dev/full")
    if not full.exists():
        return
    env = {**os.environ, "XDG_DATA_HOME": str(data)}
    # And the verb that was writing is named. Every other diagnostic here
    # carries it -- `ls: --untagged and --tag ask opposite questions` -- and
    # coreutils prints `ls: write error: ...` for this very failure, so a script
    # reading a log of a dozen commands can tell which one lost its answer.
    for args, verb in ((["ls", "--tag", "written"], "ls"),
                       (["--json", "ls", "--tag", "written"], "ls"),
                       (["tags"], "tags"),
                       (["export"], "export"),
                       (["recent"], "recent"),
                       (["tag", "ls", "--", str(subject)], "tag ls")):
        with full.open("w") as sink:
            done = subprocess.run([str(CLI), *args], cwd=ROOT, env=env, text=True,
                                  stdout=sink, stderr=subprocess.PIPE,
                                  stdin=subprocess.DEVNULL)
        assert done.returncode != 0, args
        assert f"{verb}: write error" in done.stderr, (args, done.stderr)

    # A working stdout is untouched by the check: it still exits 0 and the
    # message is not printed at every run.
    fine = run(data, "ls", "--tag", "written")
    assert fine.returncode == 0 and "write error" not in fine.stderr, fine.stderr


def check_globals(data, workspace):
    """The four flags every verb inherits, and the env vars behind them."""
    room = workspace / "globals"
    room.mkdir()
    subject = room / "g.txt"
    subject.write_text("g")
    run(data, "tag", "add", "--tag", "global", "--", str(subject))

    # --quiet silences the warning that --since prints, without hiding the rows.
    #
    # The file has to exist: the count is about the rows this query would have
    # returned, so a row whose file is gone is dropped before it is counted.
    index = data / "omafiles/index.jsonl"
    old = room / "old.txt"
    old.write_text("o")
    with index.open("a") as handle:
        handle.write(json.dumps({
            "path": str(old.resolve()), "tags": ["global"],
            }) + "\n")
    loud = run(data, "ls", "--tag", "global", "--since", "1h")
    hushed = run(data, "--quiet", "ls", "--tag", "global", "--since", "1h")
    assert "predate" in loud.stderr and hushed.stderr.strip() == ""
    assert loud.stdout == hushed.stdout

    # Short forms, and the env vars that stand in for them.
    assert json.loads(run(data, "-j", "ls", "--tag", "global").stdout)
    assert json.loads(run(data, "ls", "--tag", "global",
                          extra_env={"OMAFILES_JSON": "1"}).stdout)

    doomed = room / "kept.txt"
    doomed.write_text("k")
    run(data, "tag", "add", "--tag", "global", "--", str(doomed))
    assert run(data, "-n", "tag", "rm", "--tag", "global", "--",
               str(doomed)).returncode == 0
    assert "global" in run(data, "tag", "ls", "--", str(doomed)).stdout
    run(data, "tag", "rm", "--tag", "global", "--", str(doomed),
        extra_env={"OMAFILES_DRY_RUN": "1"})
    assert "global" in run(data, "tag", "ls", "--", str(doomed)).stdout

    # OMAFILES_YES stands in for --yes on a destructive verb.
    assert run(data, "rm", "--", str(doomed)).returncode == 1
    assert run(data, "rm", "--", str(doomed),
               extra_env={"OMAFILES_YES": "1"}).returncode == 0
    assert not doomed.exists()

    # Asking for help is not a usage error; arriving with no verb is.
    assert run(data, "--help").returncode == 0
    assert run(data, "-h").returncode == 0
    assert run(data, "--version").returncode == 0
    assert "not built" not in run(data, "--help").stdout.lower()


def check_prune_scope(data, workspace):
    """prune takes a directory, and honours it."""
    inside = workspace / "pruned"
    outside = workspace / "kept"
    inside.mkdir()
    outside.mkdir()
    here = inside / "here.txt"
    there = outside / "there.txt"
    here.write_text("h")
    there.write_text("t")
    run(data, "tag", "add", "--tag", "scoped", "--", str(here), str(there))
    here.unlink()
    there.unlink()

    dropped = run(data, "--yes", "prune", str(inside))
    assert dropped.returncode == 0, dropped.stderr
    assert str(here.resolve()) in dropped.stdout
    # The row outside the named directory is not this run's business.
    assert str(there.resolve()) not in dropped.stdout
    assert str(there.resolve()) in run(data, "export").stdout


def check_moving_a_folder(data, workspace):
    """mv and trash on a directory take the rows inside it with them.

    Moving a folder used to rewrite only the folder's own row, so every tagged
    file under it was left pointing at a tree that no longer existed -- exit 0,
    no warning, and every one of those tags lost to any query."""
    room = workspace / "moving"
    (room / "tree/sub").mkdir(parents=True)
    (room / "dest").mkdir()
    inner = room / "tree/inner.txt"
    deep = room / "tree/sub/deep.txt"
    # A sibling whose name starts with the folder's, to prove the prefix match
    # is on a path separator and not on the text.
    decoy = room / "treehouse.txt"
    for f in (inner, deep, decoy):
        f.write_text("x")
    run(data, "tag", "add", "--tag", "moved", "--",
        str(room / "tree"), str(inner), str(deep), str(decoy))

    stamp = lambda p: json.loads(run(data, "--json", "json", "--", str(p)).stdout)["tagged"]
    before = stamp(inner)

    moved = run(data, "mv", str(room / "tree"), str(room / "dest/tree"))
    assert moved.returncode == 0, moved.stderr

    rows = run(data, "export").stdout
    assert str((room / "dest/tree").resolve()) in rows
    assert str((room / "dest/tree/inner.txt").resolve()) in rows
    assert str((room / "dest/tree/sub/deep.txt").resolve()) in rows
    assert str(inner.resolve()) not in rows
    # The decoy kept its own row, untouched.
    assert str(decoy.resolve()) in rows

    # Nothing in the index points at anything missing.
    assert run(data, "verify", str(room)).returncode == 0

    # A move is not a tagging event, for every row it touches.
    assert stamp(room / "dest/tree/inner.txt") == before

    # A folder cannot be moved inside itself.
    inside = run(data, "mv", str(room / "dest/tree"), str(room / "dest/tree/here"))
    assert inside.returncode == 1
    assert "inside" in inside.stderr

    # And the trash takes the rows under a folder too, or the index keeps tags
    # for files that are in the wastebasket.
    if shutil.which("gio") is None:
        return
    env = {"HOME": str(workspace.parent / "home")}
    (workspace.parent / "home").mkdir(exist_ok=True)

    # The rows under dest/tree are already there, put by the move.
    assert str((room / "dest/tree/inner.txt").resolve()) in run(data, "export").stdout
    binned = run(data, "--yes", "trash", "--", str(room / "dest/tree"), extra_env=env)
    if binned.returncode != 0:
        return                                   # no trash reachable from here
    left = run(data, "export", extra_env=env).stdout
    assert str((room / "dest/tree/inner.txt").resolve()) not in left
    assert str((room / "dest/tree/sub/deep.txt").resolve()) not in left
    assert str(decoy.resolve()) in left


def check_contradictory_flags(data, workspace):
    """--untagged cannot be narrowed by the flags that only match tags."""
    room = workspace / "contradictions"
    room.mkdir()
    (room / "bare.txt").write_text("b")

    # It listed the untagged file when asked plainly, and nothing at all when
    # asked with --since, because an untagged row has no tag time to compare.
    # Empty and silent was indistinguishable from a folder with nothing new.
    assert run(data, "ls", "--untagged", str(room)).stdout.strip() != ""

    for extra in (["--since", "1h"], ["--tag", "anything"], ["--any", "anything"]):
        clash = run(data, "ls", "--untagged", *extra, str(room))
        assert clash.returncode == 1, extra
        assert "opposite questions" in clash.stderr, extra


def check_json_on_file_verbs(data, workspace):
    """--json means JSON on stdout, on every verb that has it."""
    room = workspace / "jsonverbs"
    room.mkdir()
    source = room / "src.txt"
    source.write_text("s")
    run(data, "tag", "add", "--tag", "shaped", "--", str(source))

    # Both used to print the destination path first, so `--json mv` emitted a
    # bare line followed by an array -- not a document anything could parse --
    # and `--json cp` emitted the line and no JSON at all.
    moved = run(data, "--json", "mv", str(source), str(room / "moved.txt"))
    assert moved.returncode == 0, moved.stderr
    answer = json.loads(moved.stdout)
    assert answer["to"] == str((room / "moved.txt").resolve())
    assert [row["path"] for row in answer["rows"]] == [str((room / "moved.txt").resolve())]

    copied = run(data, "--json", "cp", str(room / "moved.txt"), str(room / "copy.txt"))
    assert copied.returncode == 0, copied.stderr
    answer = json.loads(copied.stdout)
    assert answer["to"] == str((room / "copy.txt").resolve())
    assert [row["path"] for row in answer["rows"]] == [str((room / "copy.txt").resolve())]

    # Without it, the path is the answer.
    plain = run(data, "cp", str(room / "moved.txt"), str(room / "plain.txt"))
    assert plain.stdout.strip() == str((room / "plain.txt").resolve())

    # And cp still stamps the copy's own xattr, which it did by hand before it
    # was routed through the same commit as everything else.
    assert os.getxattr(room / "plain.txt", "user.omafiles.tags") == b"shaped"

    # The index only answers where it has a row. A store miss is not one: empty
    # stdout, exit 0 -- the same as plain `tag ls`. An empty `tags` in JSON is a
    # write receipt after `tag clear` or the last `tag rm`, not a file it never
    # indexed.
    bare = room / "bare.txt"
    bare.write_text("b")
    for verb in (("tag", "ls"), ("json",)):
        empty = run(data, "--json", *verb, "--", str(bare))
        assert empty.returncode == 0, empty.stderr
        assert empty.stdout == "", (verb, empty.stdout)

    # A folder cannot be moved into itself even through a symlink, and the
    # failure says what the system said rather than guessing at filesystems.
    real = room / "real"
    real.mkdir()
    link = room / "link"
    link.symlink_to(real)
    inside = run(data, "mv", str(link), str(link / "inner"))
    assert inside.returncode == 1
    assert "is inside" in inside.stderr, inside.stderr



def check_json_does_not_invent_a_row(data, workspace):
    """`json` and `--json tag ls` only answer where the index has a row.

    A store miss is not one: empty stdout, exit 0, like plain `tag ls`. Empty
    `tags` in JSON is a write receipt after the last tag goes away, not a file
    the index never heard of. `export` and `has` already knew that."""
    room = workspace / "norow"
    room.mkdir()
    never = room / "never.txt"
    never.write_text("n")

    cleared = room / "cleared.txt"
    cleared.write_text("c")
    assert run(data, "tag", "add", "--tag", "gone", "--", str(cleared)).returncode == 0
    assert run(data, "tag", "clear", "--", str(cleared)).returncode == 0

    fifo = room / "pipe"
    os.mkfifo(fifo)

    for subject in (never, cleared, fifo):
        resolved = str(subject.resolve())
        for verb in (("tag", "ls"), ("json",)):
            empty = run(data, "--json", *verb, "--", resolved)
            assert empty.returncode == 0, (subject.name, verb, empty.stderr)
            assert empty.stdout == "", (subject.name, verb, empty.stdout)
        plain = run(data, "tag", "ls", "--", resolved)
        assert plain.returncode == 0, plain.stderr
        assert plain.stdout == "", plain.stdout
        exported = run(data, "export").stdout
        assert resolved not in exported, (subject.name, exported)

    tagged = room / "real.txt"
    tagged.write_text("t")
    assert run(data, "tag", "add", "--tag", "kept", "--", str(tagged)).returncode == 0
    resolved_tagged = str(tagged.resolve())
    for verb in (("tag", "ls"), ("json",)):
        row = json.loads(run(data, "--json", *verb, "--", resolved_tagged).stdout)
        assert row["path"] == resolved_tagged
        assert row["tags"] == ["kept"]
        assert row["tagged"] > 0

    missing = run(data, "json", "--", str(room / "nope.txt"))
    assert missing.returncode == 2



def check_json_array_verbs_do_not_invent_a_row(data, workspace):
    """`--json tag rm` and `--json tag clear` must not invent a row on a miss.

    Array verbs still use `[]`. Empty `tags` is the receipt after a write that
    took the last tag off a path the index already had, not a file it never
    indexed."""
    room = workspace / "array-norow"
    room.mkdir()
    never = room / "never.txt"
    never.write_text("n")
    resolved_never = str(never.resolve())

    for verb in (
        ("tag", "clear"),
        ("tag", "rm", "--tag", "ghost"),
    ):
        empty = run(data, "--json", *verb, "--", str(never))
        assert empty.returncode == 0, (verb, empty.stderr)
        assert empty.stdout == "[]\n", (verb, empty.stdout)

    tagged = room / "last.txt"
    tagged.write_text("t")
    resolved_tagged = str(tagged.resolve())
    assert run(data, "tag", "add", "--tag", "only", "--", str(tagged)).returncode == 0

    cleared = json.loads(run(data, "--json", "tag", "clear", "--", str(tagged)).stdout)
    assert len(cleared) == 1, cleared
    assert cleared[0]["path"] == resolved_tagged, cleared
    assert cleared[0]["tags"] == [], cleared
    assert cleared[0]["tagged"] > 0, cleared

    assert run(data, "tag", "add", "--tag", "only", "--", str(tagged)).returncode == 0
    removed = json.loads(
        run(data, "--json", "tag", "rm", "--tag", "only", "--", str(tagged)).stdout
    )
    assert len(removed) == 1, removed
    assert removed[0]["path"] == resolved_tagged, removed
    assert removed[0]["tags"] == [], removed
    assert removed[0]["tagged"] > 0, removed

def check_concurrent_writers(data, workspace):
    """Concurrent taggers never lose a tag in silence.

    The lock used to cover only the write, so every process read the same index,
    added its row to its own copy, and the last to commit won -- thirty-nine of
    forty tags vanished with every process exiting 0 and printing its row.

    The invariant is not "all forty succeed": under enough contention a writer
    may legitimately give up waiting for the lock. It is that a writer either
    succeeds and its tag is there, or fails loudly. Silence with a missing tag
    is the bug."""
    room = workspace / "concurrent"
    room.mkdir()
    env = {**os.environ, "XDG_DATA_HOME": str(data)}

    def race(jobs):
        running = [
            (tag, path, subprocess.Popen(
                [str(CLI), "tag", "add", "--tag", tag, "--", str(path)],
                cwd=ROOT, env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))
            for tag, path in jobs
        ]
        done = []
        for tag, path, process in running:
            _, err = process.communicate()
            done.append((tag, path, process.returncode, err.decode()))
        return done

    files = []
    for i in range(40):
        f = room / f"f{i}.txt"
        f.write_text("x")
        files.append(f)

    outcome = race([(f"race{i}", f) for i, f in enumerate(files)])
    tags_now = run(data, "export").stdout
    for tag, path, code, err in outcome:
        if code == 0:
            assert str(path.resolve()) in tags_now, f"{path.name} exited 0 and is not indexed"
        else:
            # Giving up on the lock is allowed; doing so quietly is not.
            assert err.strip(), f"{path.name} failed with nothing on stderr"

    survived = sum(1 for _, _, code, _ in outcome if code == 0)
    assert survived >= 2, "no concurrent write got through at all"

    # A separate, small race that must succeed outright. The forty above assert
    # only that nothing is lost silently, which stays true if the lock timeout
    # is cut to a millisecond and three quarters of the writers give up -- a
    # 35% failure rate the suite could not see. Eight writers fit inside the
    # real timeout even with every other suite running, and do not fit inside
    # a broken one.
    modest = []
    for i in range(8):
        f = room / f"modest{i}.txt"
        f.write_text("x")
        modest.append(f)
    outcome = race([(f"modest{i}", f) for i, f in enumerate(modest)])
    for tag, path, code, err in outcome:
        assert code == 0, f"{path.name} could not take the lock in time: {err}"

    # The same file from many processes accumulates rather than overwrites.
    shared = room / "shared.txt"
    shared.write_text("s")
    outcome = race([(f"heap{i}", shared) for i in range(20)])
    landed = set(run(data, "tag", "ls", "--", str(shared)).stdout.split())
    for tag, _, code, err in outcome:
        if code == 0:
            assert tag in landed, f"{tag} exited 0 and is not on the file"
        else:
            assert err.strip(), f"{tag} failed with nothing on stderr"
    assert len(landed) >= 2, "concurrent writes to one file did not accumulate"


def check_verify_walks_the_disk(data, workspace):
    """verify compares index AND disk, which is what its help says."""
    room = workspace / "verifying"
    room.mkdir()
    stray = room / "stray.txt"
    stray.write_text("s")

    # A file carrying tags the index never heard of. Walking only the index made
    # this invisible, which is what made a lost write undetectable.
    os.setxattr(stray, "user.omafiles.tags", b"orphan")
    found = run(data, "verify", str(room))
    assert found.returncode == 1, found.stdout
    assert "unindexed" in found.stdout and str(stray.resolve()) in found.stdout

    rows = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
    assert any(row["problem"] == "unindexed" for row in rows)

    # Once the index knows, it is clean again.
    run(data, "reindex", str(room))
    assert run(data, "verify", str(room)).returncode == 0

    # A directory carrying tags the index never heard of. `tag add -- somedir`
    # is a documented, tested feature, so a walk asking only for QDir::Files
    # left the one kind of tagged thing it cannot see exactly as invisible as
    # walking only the index had left everything.
    folder = room / "tagged-dir"
    folder.mkdir()
    os.setxattr(folder, "user.omafiles.tags", b"strayfolder")
    found = run(data, "verify", str(room))
    assert found.returncode == 1, found.stdout
    assert str(folder.resolve()) in found.stdout, found.stdout

    # And reindex can rebuild its row, which it could not while it walked past.
    assert run(data, "reindex", str(room)).returncode == 0
    assert run(data, "tag", "ls", "--", str(folder)).stdout.split() == ["strayfolder"]

    # A dotfile is a file somebody chose to tag. Same walk, same answer.
    hidden = room / ".hidden.txt"
    hidden.write_text("h")
    os.setxattr(hidden, "user.omafiles.tags", b"dotted")
    assert str(hidden.resolve()) in run(data, "verify", str(room)).stdout

    # The directory you actually named. QDirIterator enumerates what is inside
    # a directory and never the directory itself, so `verify somedir` walked
    # past the one thing it was pointed at and called the tree clean -- which is
    # worse than saying nothing, because it is an answer.
    named = room / "named"
    named.mkdir()
    os.setxattr(named, "user.omafiles.tags", b"itsown")
    walked = run(data, "verify", str(named))
    assert walked.returncode == 1, walked.stdout
    assert str(named.resolve()) in walked.stdout, walked.stdout
    assert run(data, "reindex", str(named)).returncode == 0
    assert run(data, "tag", "ls", "--", str(named)).stdout.split() == ["itsown"]

    # A symlink and its target are one file. Both turn up in a walk, and a walk
    # that does not notice reports the same problem twice and -- through
    # reindex, the one writer that never went through resolveExisting -- writes
    # two index rows for one inode, so `tags` counts it twice and `tag ls` can
    # only find it under one of the two names.
    linked = room / "linked"
    linked.mkdir()
    target = linked / "real.txt"
    target.write_text("r")
    os.symlink("real.txt", linked / "link.txt")
    os.setxattr(target, "user.omafiles.tags", b"linkedtag")
    reported = run(data, "verify", str(linked)).stdout.splitlines()
    assert len(reported) == 1, reported
    assert run(data, "reindex", str(linked)).returncode == 0
    rows = [line for line in run(data, "export").stdout.splitlines() if "linkedtag" in line]
    assert len(rows) == 1, rows
    assert run(data, "verify", str(linked)).returncode == 0

    # A symlink pointing out of the tree is not part of the tree. Canonicalising
    # every entry made the walk answer about wherever the link went: `verify`
    # reported a file outside the root as unindexed, and `reindex` could not
    # settle it, because the row it wrote was outside the root too and the next
    # `verify` filtered it back out and found the file all over again.
    away = workspace / "elsewhere"
    away.mkdir(exist_ok=True)
    beyond = away / "beyond.txt"
    beyond.write_text("b")
    os.setxattr(beyond, "user.omafiles.tags", b"beyondtag")
    escape = room / "escapes"
    escape.mkdir()
    os.symlink(beyond, escape / "out.txt")
    quiet = run(data, "verify", str(escape))
    assert quiet.returncode == 0, quiet.stdout
    assert str(beyond.resolve()) not in quiet.stdout, quiet.stdout
    assert run(data, "reindex", str(escape)).returncode == 0
    assert str(beyond.resolve()) not in run(data, "export").stdout, "reindex followed the link out"
    assert run(data, "verify", str(escape)).returncode == 0, "verify never settles"
    assert run(data, "ls", "--untagged", str(escape)).stdout.strip() == ""

    # A directory nobody may open is not an empty directory. QDirIterator walks
    # past it without a word, so everything under it went unexamined and the
    # answer came back clean -- and silence from a verb whose whole job is to
    # say whether two things agree reads as agreement.
    shut = room / "shut"
    shut.mkdir()
    inside = shut / "hidden.txt"
    inside.write_text("h")
    os.setxattr(inside, "user.omafiles.tags", b"unseen")
    shut.chmod(0o000)
    try:
        blind = run(data, "verify", str(room))
        assert blind.returncode == 1, blind.stdout
        assert "unreadable" in blind.stdout and str(shut.resolve()) in blind.stdout, blind.stdout
        kinds = [row["problem"] for row
                 in json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]]
        assert "unreadable" in kinds, kinds
        # One line per cause, not one per consequence.
        assert kinds.count("unreadable") == 1, kinds
        assert "xattr-unreadable" not in kinds, kinds
    finally:
        shut.chmod(0o755)

    # And a single file nobody may read. Same distinction, one level down: the
    # disk side threw the warning away, so a file whose xattr could not be read
    # was indistinguishable from a file that has none.
    sealed = room / "sealed.txt"
    sealed.write_text("s")
    os.setxattr(sealed, "user.omafiles.tags", b"sealedtag")
    sealed.chmod(0o000)
    try:
        denied = run(data, "verify", str(room))
        assert denied.returncode == 1
        assert "xattr-unreadable" in denied.stdout, denied.stdout
        assert str(sealed.resolve()) in denied.stdout, denied.stdout
    finally:
        sealed.chmod(0o644)

    # The other problem kind. Nothing asserted `missing` before, so verify could
    # stop reporting a row whose file is gone and no test would notice.
    run(data, "reindex", str(room))
    stray.unlink()
    gone = run(data, "verify", str(room))
    assert gone.returncode == 1
    assert "missing" in gone.stdout and str(stray.resolve()) in gone.stdout
    kinds = {row["problem"]
             for row in json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]}
    assert "missing" in kinds, kinds


def check_verify_answers_one_question(data, workspace):
    """`verify` and `verify .` are the same question.

    The disk walk fell back to the current directory while the index side, given
    no directory, checked every row wherever it pointed -- so the two documented
    spellings of "here" disagreed."""
    room = workspace / "scoped"
    (room / "away").mkdir(parents=True)
    (room / "here").mkdir()
    stray = room / "away" / "f.txt"
    stray.write_text("f")
    run(data, "tag", "add", "--tag", "scopecheck", "--", str(stray))
    stray.unlink()

    bare = subprocess.run([str(CLI), "verify"], cwd=room / "here",
                          env={**os.environ, "XDG_DATA_HOME": str(data)},
                          text=True, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    dotted = subprocess.run([str(CLI), "verify", "."], cwd=room / "here",
                            env={**os.environ, "XDG_DATA_HOME": str(data)},
                            text=True, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert bare.returncode == dotted.returncode, (bare.returncode, dotted.returncode)
    assert bare.stdout == dotted.stdout, (bare.stdout, dotted.stdout)
    assert bare.returncode == 0, bare.stdout

    # Both of them still see what is under the directory they mean.
    run(data, "--yes", "prune", "--all")


def check_a_file_moved_behind_our_back(data, workspace):
    """A file moved without omafiles is recovered by reindex and prune.

    The index knows a file by its absolute path, so a `mv` in a shell or a file
    manager leaves the row pointing at nothing. Nothing is lost, because the tags
    live in two places: the row and the file's own xattr, and a rename inside one
    filesystem is the same inode under a new name, so the xattr travels with it.
    This is the recovery, asserted rather than assumed -- it was the argument for
    not storing an inode to detect the move, so it had better be true."""
    room = workspace / "movedaway"
    (room / "before").mkdir(parents=True)
    (room / "after").mkdir()
    subject = room / "before" / "m.txt"
    subject.write_text("m")
    assert run(data, "tag", "add", "--tag", "travels", "--", str(subject)).returncode == 0
    stamp = json.loads(run(data, "--json", "json", "--", str(subject)).stdout)["tagged"]

    moved = room / "after" / "m.txt"
    os.rename(subject, moved)                    # behind the program's back

    # The file took its own copy of the tags along.
    assert os.getxattr(moved, "user.omafiles.tags").decode() == "travels"

    # verify says both halves of the truth: a row pointing nowhere, and a file
    # carrying tags no row knows about.
    torn = run(data, "verify", str(room))
    assert torn.returncode == 1
    kinds = {row["problem"]: row["path"] for row
             in json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]}
    assert kinds.get("missing") == str(subject.resolve()), kinds
    assert kinds.get("unindexed") == str(moved.resolve()), kinds

    # reindex reads the file's own copy and writes the row at the new path.
    assert run(data, "reindex", str(room)).returncode == 0
    assert run(data, "tag", "ls", "--", str(moved)).stdout.split() == ["travels"]
    assert str(moved.resolve()) in run(data, "ls", "--tag", "travels").stdout

    # prune drops the row that points nowhere, and then the tree agrees again.
    dropped = run(data, "--yes", "prune", str(room))
    assert dropped.returncode == 0, dropped.stderr
    assert str(subject.resolve()) in dropped.stdout, dropped.stdout
    assert run(data, "verify", str(room)).returncode == 0

    # The one thing that does not survive: the row is new, so the tagging stamp
    # is new. `recent` treats a recovered file as freshly tagged, and saying so
    # here is cheaper than someone discovering it.
    assert json.loads(run(data, "--json", "json", "--", str(moved)).stdout)["tagged"] >= stamp


def check_import_canonicalises(data, workspace):
    """import was the last write path that took a path as written.

    A dump naming a file through a symlinked parent produced a second row for a
    file that already had one: `tags` counted it twice, `tag ls` found it under
    only one of the two names, and `verify` called the file unindexed while its
    row sat there under the other spelling."""
    room = workspace / "importlink"
    (room / "real").mkdir(parents=True)
    subject = room / "real" / "g.txt"
    subject.write_text("g")
    os.symlink("real", room / "through")

    dump = room / "dump.jsonl"
    dump.write_text(json.dumps({
        "path": str(room / "through" / "g.txt"), "tags": ["linkimport"],
    }) + "\n")
    assert run(data, "import", str(dump)).returncode == 0

    rows = [line for line in run(data, "export").stdout.splitlines() if "linkimport" in line]
    assert len(rows) == 1, rows
    assert str(subject.resolve()) in rows[0], rows[0]
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["linkimport"]
    assert run(data, "verify", str(room)).returncode == 0

    # And reindex over the same tree does not add a second one.
    assert run(data, "reindex", str(room)).returncode == 0
    assert len([l for l in run(data, "export").stdout.splitlines() if "linkimport" in l]) == 1

    # A row for a file that is not on this machine is still importable: a dump
    # from somewhere else is a real thing to merge.
    absent = room / "dump2.jsonl"
    absent.write_text(json.dumps({
        "path": "/nowhere/at/all/x.txt", "tags": ["remote"],
    }) + "\n")
    assert run(data, "import", str(absent)).returncode == 0
    assert "/nowhere/at/all/x.txt" in run(data, "export").stdout


def check_the_disk_is_never_ahead_of_the_index(data, workspace):
    """The file's xattr is written after the commit, never before.

    It used to be stamped as soon as the verb had the row. A commit that then
    failed -- a read-only index is the easiest way to see it -- left the file
    claiming a tag no row ever recorded: exactly the disagreement `verify`
    exists to shout about, manufactured by the program itself."""
    room = workspace / "ordering"
    room.mkdir()
    subject = room / "b.txt"
    subject.write_text("b")
    assert run(data, "tag", "add", "--tag", "landed", "--", str(subject)).returncode == 0

    index = data / "omafiles/index.jsonl"
    index.chmod(0o400)
    try:
        blocked = run(data, "tag", "add", "--tag", "doomed", "--", str(subject))
    finally:
        index.chmod(0o600)
    assert blocked.returncode != 0, "a write to a read-only index reported success"
    assert blocked.stderr.strip()

    on_disk = os.getxattr(subject, "user.omafiles.tags").decode().split(",")
    assert "doomed" not in on_disk, f"the file was stamped for a commit that failed: {on_disk}"
    assert run(data, "verify", str(room)).returncode == 0, "the failed write left a disagreement"

    # The same in reverse: `tag clear` empties the xattr, and must not empty it
    # for a commit that never lands.
    index.chmod(0o400)
    try:
        run(data, "tag", "clear", "--", str(subject))
    finally:
        index.chmod(0o600)
    assert os.getxattr(subject, "user.omafiles.tags").decode() == "landed"

    # The repair has to work. Skipping the whole write when the tag set does not
    # change also skipped the xattr, so a file whose own copy had gone missing
    # -- a `cp` without --preserve=xattr -- could not be put right by the one
    # command anybody would reach for, and `tag add` exited 0 having done
    # nothing while `verify` went on complaining.
    stamp = json.loads(run(data, "--json", "json", "--", str(subject)).stdout)["tagged"]
    os.removexattr(subject, "user.omafiles.tags")
    assert run(data, "verify", str(room)).returncode == 1
    time.sleep(1.1)
    again = run(data, "tag", "add", "--tag", "landed", "--", str(subject))
    assert again.returncode == 0, again.stderr
    assert os.getxattr(subject, "user.omafiles.tags").decode() == "landed"
    assert run(data, "verify", str(room)).returncode == 0, "tag add did not repair the file"
    # And repairing it is not a tagging event.
    assert json.loads(run(data, "--json", "json", "--", str(subject)).stdout)["tagged"] == stamp

    # `tag rm` is the same shape and had the same hole: removing a tag the file
    # never had changes no tag, but it still rewrites the file's own copy.
    os.removexattr(subject, "user.omafiles.tags")
    removed = run(data, "tag", "rm", "--tag", "neverthere", "--", str(subject))
    assert removed.returncode == 0, removed.stderr
    assert os.getxattr(subject, "user.omafiles.tags").decode() == "landed"
    assert json.loads(run(data, "--json", "json", "--", str(subject)).stdout)["tagged"] == stamp

    # Both of them still answer as JSON. Skipping the write printed `[]` for a
    # path that has a row, which is not what "give me what you wrote" means.
    for verb in (("tag", "add", "--tag", "landed"), ("tag", "rm", "--tag", "neverthere")):
        spoken = run(data, "--json", *verb, "--", str(subject))
        assert json.loads(spoken.stdout), f"{verb} answered with nothing" 


def check_prune_asks(data, workspace):
    """prune is declared destructive, so it behaves like the other two.

    A row whose file is not there right now is not always a row for a file that
    is gone: an unplugged drive reads the same way, and the tags it drops exist
    nowhere else."""
    room = workspace / "pruning"
    room.mkdir()
    subject = room / "p.txt"
    subject.write_text("p")
    run(data, "tag", "add", "--tag", "pruned", "--", str(subject))
    subject.unlink()

    refused = run(data, "prune", str(room))
    assert refused.returncode == 1, refused.stdout
    assert "--yes" in refused.stderr, refused.stderr
    assert str(subject.resolve()) in run(data, "export").stdout, "prune dropped the row anyway"

    assert run(data, "--yes", "prune", str(room)).returncode == 0
    assert str(subject.resolve()) not in run(data, "export").stdout

    # Nothing to drop is not a question worth asking.
    assert run(data, "prune", str(room)).returncode == 0

    # Bare `prune` means the directory you are standing in, like `verify` and
    # `reindex` do -- it used to mean the whole index, which made the one
    # destructive verb here the one with the widest reach. `--all` is that reach,
    # spelled out.
    outside = workspace / "pruning-elsewhere"
    outside.mkdir()
    far = outside / "far.txt"
    far.write_text("f")
    run(data, "tag", "add", "--tag", "pruned", "--", str(far))
    far.unlink()
    here = room / "near.txt"
    here.write_text("n")
    run(data, "tag", "add", "--tag", "pruned", "--", str(here))
    here.unlink()

    standing = subprocess.run([str(CLI), "--yes", "prune"], cwd=room,
                              env={**os.environ, "XDG_DATA_HOME": str(data)},
                              text=True, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert standing.returncode == 0, standing.stderr
    assert str(here.resolve()) in standing.stdout, standing.stdout
    assert str(far.resolve()) not in standing.stdout, "bare prune reached outside the directory"
    assert str(far.resolve()) in run(data, "export").stdout

    # And --all does reach it.
    everywhere = run(data, "--yes", "prune", "--all")
    assert everywhere.returncode == 0, everywhere.stderr
    assert str(far.resolve()) in everywhere.stdout
    assert str(far.resolve()) not in run(data, "export").stdout

    # --all is the whole index by definition, so naming a directory too is a
    # contradiction rather than a narrowing.
    both = run(data, "prune", "--all", str(room))
    assert both.returncode == 1
    assert "takes no directory" in both.stderr, both.stderr

    # A flag is only accepted by a verb that has it. `--from-xattr` and `--all`
    # used to be taken from anywhere, so `ls --from-xattr` was a typo the
    # program agreed with.
    for verb, flag in (("ls", "--from-xattr"), ("ls", "--all"),
                       ("tags", "--all"), ("verify", "--from-xattr")):
        stray = run(data, verb, flag)
        assert stray.returncode == 1, f"{verb} {flag} was accepted"
        assert flag in stray.stderr, stray.stderr

    # And inside a batch, where there is nobody to ask. It refuses rather than
    # guessing, and says the same thing it says anywhere else.
    gone = room / "q.txt"
    gone.write_text("q")
    run(data, "tag", "add", "--tag", "pruned", "--", str(gone))
    gone.unlink()
    batched = run(data, "batch", "-", stdin=f"prune {room}\n")
    assert batched.returncode != 0
    assert "--yes" in batched.stderr, batched.stderr
    assert str(gone.resolve()) in run(data, "export").stdout, "the batch pruned anyway"
    # With the answer given for the whole run, it goes through.
    assert run(data, "--yes", "batch", "-", stdin=f"prune {room}\n").returncode == 0
    assert str(gone.resolve()) not in run(data, "export").stdout


def check_a_write_that_changes_nothing(data, workspace):
    """Adding a tag a file has, or removing one it does not, is not an event.

    `recent` is ordered by when a path was last tagged, and restamping the row
    for a write that changed no tag moved a file to the top of that answer for
    having been asked a question."""
    room = workspace / "noop"
    room.mkdir()
    subject = room / "n.txt"
    subject.write_text("n")
    run(data, "tag", "add", "--tag", "steady", "--", str(subject))

    def stamp():
        return json.loads(run(data, "--json", "json", "--", str(subject)).stdout)["tagged"]

    before = stamp()
    time.sleep(1.1)                              # the stamp has second resolution
    assert run(data, "tag", "rm", "--tag", "notthere", "--", str(subject)).returncode == 0
    assert stamp() == before, "removing a tag that was not there restamped the row"
    assert run(data, "tag", "add", "--tag", "steady", "--", str(subject)).returncode == 0
    assert stamp() == before, "adding a tag the file already had restamped the row"

    # A real change still counts.
    assert run(data, "tag", "add", "--tag", "fresh", "--", str(subject)).returncode == 0
    assert stamp() > before
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["fresh", "steady"]


def check_import_validates(data, workspace):
    """import is a write path, so it validates like the others."""
    room = workspace / "importing"
    room.mkdir()
    subject = room / "i.txt"
    subject.write_text("i")

    def dump(tags):
        f = room / "d.jsonl"
        f.write_text(json.dumps({
            "path": str(subject.resolve()), "tags": tags,
            }) + "\n")
        return f

    # `tag add` refuses these; import used to take them, and then `tag rm`
    # could never match them because it normalises before comparing. Case is
    # not among them: `tag add --tag UPPER` lowercases rather than refusing, and
    # import has to agree.
    for bad in (["Has Space!"], ["-lead"], ["a" * 65]):
        refused = run(data, "import", str(dump(bad)))
        assert refused.returncode == 1, bad
        assert "invalid tag" in refused.stderr, bad

    # A comma is the xattr's separator, so one imported tag came back from
    # reindex as two.
    comma = run(data, "import", str(dump(["red,green"])))
    assert comma.returncode == 1
    assert "invalid tag" in comma.stderr

    # Nothing was written by any of them.
    assert run(data, "tag", "ls", "--", str(subject)).stdout.strip() == ""

    # `-` is stdin, the way it is for `batch`. `export` writes the dump to
    # stdout and this reads one, so the pair is a pipe -- and it answered
    # `unknown flag: -`, because `parseFlags` cannot tell the name of stdin
    # from the start of a flag.
    piped = run(data, "import", "-", stdin=dump(["piped"]).read_text())
    assert piped.returncode == 0, piped.stderr
    assert "piped" in run(data, "tag", "ls", "--", str(subject)).stdout.split()
    run(data, "tag", "clear", "--", str(subject))

    good = run(data, "import", str(dump(["fine", "Also-Fine"])))
    assert good.returncode == 0, good.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["also-fine", "fine"]

    # And what it stored is what the xattr carries, so a reindex agrees with it.
    assert os.getxattr(subject, "user.omafiles.tags") == b"also-fine,fine"

    # `tagged` is told apart from an absent one, the way `path` and `tags` are.
    # It went through `toVariant().toLongLong()`, which takes anything at all:
    # a string became 0, an undated row that `recent` and `--since` can never
    # judge, and `true` became 1 -- a file tagged in January 1970. Both landed
    # in the index at exit 0.
    stamped = room / "t.txt"
    stamped.write_text("t")

    def dated(tagged):
        f = room / "s.jsonl"
        f.write_text(json.dumps({
            "path": str(stamped.resolve()), "tags": ["dated"], "tagged": tagged,
            }) + "\n")
        return f

    for bad in ("nope", True, [], {}, None):
        refused = run(data, "import", str(dated(bad)))
        assert refused.returncode == 1, bad
        assert "non-numeric tagged" in refused.stderr, (bad, refused.stderr)
    assert run(data, "tag", "ls", "--", str(stamped)).stdout.strip() == ""

    # A number still imports, and it is the stamp the row keeps.
    when = run(data, "import", str(dated(1700000000)))
    assert when.returncode == 0, when.stderr
    row = json.loads(run(data, "--json", "json", "--", str(stamped)).stdout)
    assert row["tagged"] == 1700000000, row

    # An absent one is still absent, not a refusal: rows written before the
    # field existed load unchanged.
    old_shape = room / "o.jsonl"
    old_shape.write_text(json.dumps({
        "path": str(stamped.resolve()), "tags": ["undated"],
        }) + "\n")
    assert run(data, "import", str(old_shape)).returncode == 0


def check_recent_counts_the_query(data, workspace):
    """recent's skipped-row count is about rows it would have listed.

    Measured as a difference, not an absolute: every check in this file shares
    one index, and several of them leave stamp-less rows behind."""
    room = workspace / "recency"
    room.mkdir()

    def skipped():
        note = run(data, "recent").stderr
        if "predate" not in note:
            return 0
        return int(note.split("recent: ")[1].split(" row")[0])

    before = skipped()

    # Five stale rows with no stamp, for files that are gone. recent would never
    # have listed them, so it must not report them as a problem with the query.
    index = data / "omafiles/index.jsonl"
    with index.open("a") as handle:
        for i in range(5):
            handle.write(json.dumps({
                "path": str((room / f"ghost{i}.txt").resolve()), "tags": ["recency"],
                    }) + "\n")
    assert skipped() == before, "recent counted rows whose file is gone"

    # A stamp-less row whose file is there is a real answer it cannot give.
    present = room / "present.txt"
    present.write_text("p")
    with index.open("a") as handle:
        handle.write(json.dumps({
            "path": str(present.resolve()), "tags": ["recency"],
            }) + "\n")
    assert skipped() == before + 1

    # And a stamped row is listed. Not asserted as the first line, and not
    # inside the default window of twenty: every row this whole run tagged in
    # the same wall-clock second ties with it, the tie is broken by path, and
    # under load the forty racers above are enough of them to push it off the
    # end of a twenty-row answer. Ask for more than the run can produce.
    live = room / "live.txt"
    live.write_text("l")
    run(data, "tag", "add", "--tag", "recency", "--", str(live))
    assert str(live.resolve()) in run(data, "recent", "500").stdout.splitlines()


def declared_verbs():
    """Every command the usage file declares, as the help would spell it.

    The kdl is what docs/cli.md and the completions are already generated from,
    so measuring the help against it is measuring the help against the one
    declaration everything else in the program agrees on."""
    verbs, parent = [], None
    for line in (ROOT / "omafiles.usage.kdl").read_text().splitlines():
        found = re.match(r"^(\s*)cmd \"([^\"]+)\"", line)
        if not found:
            continue
        indent, name = len(found.group(1)), found.group(2)
        if indent == 0:
            parent = name
            verbs.append(name)
        else:
            if parent in verbs:
                verbs.remove(parent)          # `tag` is spelled by its children
            verbs.append(f"{parent} {name}")
    return verbs


def listed_verbs(text):
    """What the help actually offers, read the way a reader reads it: the verb
    is what a help line starts with."""
    found = set()
    for line in text.splitlines():
        words = line.split()
        if not words:
            continue
        found.add(f"{words[0]} {words[1]}" if words[0] == "tag" and len(words) > 1
                  else words[0])
    return found


def check_one_version(data, workspace):
    """Every place that says a version says the same one.

    It used to be written out five times -- three in the CLI's main.cpp alone.
    Four of those now share a compile-time define; the fifth is the usage
    declaration, which feeds docs/cli.md and has to stay a literal. So the two
    halves that cannot share a source are compared here instead."""
    spoken = run(data, "--version")
    assert spoken.returncode == 0, spoken.stderr
    version = spoken.stdout.split()[-1]
    assert re.fullmatch(r"\d+\.\d+\.\d+", version), spoken.stdout

    declared = re.search(r'^version "([^"]+)"',
                         (ROOT / "omafiles.usage.kdl").read_text(), re.M)
    assert declared, "omafiles.usage.kdl declares no version"
    assert declared.group(1) == version, (declared.group(1), version)

    # And the help agrees with `--version`, which is the copy a reader sees.
    assert run(data, "--help").stdout.splitlines()[0].split()[-1] == version


def check_help_and_messages(data, workspace):
    """The help lists what is built, and failures say what went wrong."""
    helped = run(data, "--help")
    assert helped.returncode == 0
    # Not `verb in stdout`: that took "tag add" apart and looked for "tag",
    # which is a substring of "--tag", of "tags", and of the flag list -- so
    # deleting the entire Tags section from the help left this green.
    offered = listed_verbs(helped.stdout)
    missing = [verb for verb in declared_verbs() if verb not in offered]
    assert not missing, f"declared but not in --help: {missing}"
    assert "not built" not in helped.stdout.lower()

    room = workspace / "messages"
    room.mkdir()
    (room / "one.txt").write_text("1")

    # The prompt counts rows, not arguments: `trash somedir` named one path and
    # dropped every row beneath it.
    sub = room / "sub"
    sub.mkdir()
    (sub / "deep.txt").write_text("d")
    run(data, "tag", "add", "--tag", "counted", "--", str(sub), str(sub / "deep.txt"))
    refused = run(data, "trash", "--", str(sub))
    assert refused.returncode == 1
    assert "2 index row(s)" in refused.stderr, refused.stderr

    # The one failure every machine can be made to reproduce: gio not on PATH.
    # Its message names the thing to install; collapsing the whole branch to
    # "trash: failed" used to pass, because nothing read it.
    (room / "gone.txt").write_text("g")
    nowhere = room / "emptypath"
    nowhere.mkdir()
    without = run(data, "--yes", "trash", "--", str(room / "gone.txt"),
                  extra_env={"PATH": str(nowhere)})
    assert without.returncode != 0
    assert "gio is not installed" in without.stderr, without.stderr
    assert (room / "gone.txt").exists(), "trash removed the file without gio"

    # A rename failure reports what the system said, not a guess about
    # filesystems.
    blocked = run(data, "mv", str(room / "one.txt"), "/proc/nope/x")
    assert blocked.returncode == 1
    assert "filesystem" not in blocked.stderr.lower(), blocked.stderr

    # cp explains a directory the way rm does.
    assert "is a directory" in run(data, "cp", str(sub), str(room / "copy")).stderr


def check_a_link_is_not_its_target(data, workspace):
    """rm, trash and mv act on the link they were handed, never through it.

    All three ran every argument through `resolveExisting`, which resolves the
    last component of a path as well as its prefix -- so `rm link.txt` unlinked
    the file at the far end of the link, exit 0, and left the link behind
    dangling. The file the user named survived and the one they did not was
    destroyed. POSIX `unlink()` removes the link; coreutils `rm` and `mv` and
    `gio trash` all act on the link, and so do these now."""
    room = workspace / "links"
    (room / "real").mkdir(parents=True)
    (room / "dest").mkdir()
    target = room / "real/t.txt"
    target.write_text("target")
    run(data, "tag", "add", "--tag", "linked", "--", str(target))

    doomed = room / "doomed.txt"
    doomed.symlink_to(target)
    removed = run(data, "--yes", "rm", str(doomed))
    assert removed.returncode == 0, removed.stderr
    assert not doomed.is_symlink(), "the link the user named survived"
    assert target.exists(), "rm followed the link and deleted the target"
    assert removed.stdout.strip() == str(doomed), removed.stdout
    assert run(data, "tag", "ls", "--", str(target)).stdout.split() == ["linked"]

    # And the three answers still agree: the index, the xattr and the disk.
    assert run(data, "verify", str(room)).returncode == 0

    # A rename moves the link, and it keeps the link's own name at the far end
    # -- `mv link.txt dest/` lands `dest/link.txt`, not `dest/<target name>`.
    travelling = room / "travelling.txt"
    travelling.symlink_to(target)
    moved = run(data, "mv", str(travelling), str(room / "dest"))
    assert moved.returncode == 0, moved.stderr
    landed = room / "dest/travelling.txt"
    assert landed.is_symlink(), "mv moved the target instead of the link"
    assert os.readlink(landed) == str(target)
    assert target.exists() and not travelling.is_symlink()
    assert moved.stdout.strip() == str(landed), moved.stdout
    assert run(data, "verify", str(room)).returncode == 0

    # A dangling link is a path that is there. `exists()` follows it and says
    # no, so `rm` on a broken link used to refuse a file plainly in the folder.
    broken = room / "broken.txt"
    broken.symlink_to(room / "never.txt")
    swept = run(data, "--yes", "rm", str(broken))
    assert swept.returncode == 0, swept.stderr
    assert not broken.is_symlink()

    # `cp` is the one that still reads through the link, because that is what
    # POSIX says an unadorned `cp` does and what coreutils writes: a plain file
    # holding the target's bytes, filed under the name that was typed. So the
    # tags come from the file the bytes came from.
    reading = room / "reading.txt"
    reading.symlink_to(target)
    copied = run(data, "cp", str(reading), str(room / "dest"))
    assert copied.returncode == 0, copied.stderr
    landed = room / "dest/reading.txt"
    assert landed.is_file() and not landed.is_symlink(), "cp copied the link itself"
    assert landed.read_text() == "target"
    assert run(data, "tag", "ls", "--", str(landed)).stdout.split() == ["linked"]
    assert run(data, "verify", str(room)).returncode == 0

    if shutil.which("gio") is None:
        return
    home = workspace.parent / "linkhome"
    home.mkdir(exist_ok=True)
    binned = room / "binned.txt"
    binned.symlink_to(target)
    trashed = run(data, "--yes", "trash", str(binned),
                  extra_env={"HOME": str(home), "XDG_DATA_HOME": str(data)})
    if trashed.returncode != 0:
        return                                   # no trash reachable from here
    assert not binned.is_symlink()
    assert target.exists(), "trash followed the link and binned the target"


def check_a_symlink_destination_is_occupied(data, workspace):
    """A link at the destination is something there, dangling or not.

    The clobber guard asked `QFileInfo::exists`, which follows the link, so a
    dangling one read as free space: `mv s.txt dang` wrote the bytes through to
    whatever `dang` pointed at, filed the index row under `dang`, and left the
    xattr on a third path -- and `verify`, run straight after that successful
    `mv`, reported the target as unindexed. Refusing is the rule these verbs
    already apply to a regular file that is there, and coreutils will not write
    through a dangling link either: `cp` says so in as many words."""
    room = workspace / "dangling"
    room.mkdir()
    subject = room / "s.txt"
    subject.write_text("s")
    run(data, "tag", "add", "--tag", "dangled", "--", str(subject))
    dang = room / "dang"
    dang.symlink_to(room / "gone.txt")

    refused = run(data, "mv", str(subject), str(dang))
    assert refused.returncode == 1, refused.stdout
    assert "already exists" in refused.stderr, refused.stderr
    assert not (room / "gone.txt").exists(), "the bytes went through the link"
    assert subject.exists(), "the source moved anyway"
    assert dang.is_symlink(), "the link was replaced"

    # The three answers still agree, which is what the bug destroyed.
    assert run(data, "verify", str(room)).returncode == 0

    assert run(data, "cp", str(subject), str(dang)).returncode == 1
    assert not (room / "gone.txt").exists(), "cp wrote through the dangling link"

    # A link that is not dangling was already refused, and still is.
    live = room / "live"
    live.symlink_to(subject)
    assert run(data, "mv", str(subject), str(live)).returncode == 1


def check_the_prompt_names_the_paths(data, workspace):
    """The question says which paths, not just how many.

    `rm: 1 path(s). Continue?` is the same sentence whether the path is the one
    you typed or the target a symlink led to, which is exactly why `rm` deleting
    a link's target went unnoticed: the prompt agreed with you either way.
    `rm -i` has named the file -- `remove regular file 'a.txt'?` -- for as long
    as it has asked. The row count stays beside it, because one path can carry a
    tree of rows."""
    room = workspace / "asking"
    room.mkdir()
    named = room / "named.txt"
    named.write_text("n")

    refused = run(data, "rm", str(named))
    assert refused.returncode == 1
    assert str(named.resolve()) in refused.stderr, refused.stderr
    assert named.exists()

    # And the count it already gave is still there: `trash somedir` names one
    # path and drops every row beneath it.
    sub = room / "sub"
    sub.mkdir()
    (sub / "deep.txt").write_text("d")
    run(data, "tag", "add", "--tag", "asked", "--", str(sub), str(sub / "deep.txt"))
    counted = run(data, "trash", "--", str(sub))
    assert "2 index row(s)" in counted.stderr, counted.stderr
    assert str(sub.resolve()) in counted.stderr, counted.stderr

    # Capped, so a long argument list cannot scroll the question itself off the
    # screen -- and the ones it did not name are counted rather than dropped.
    many = [room / f"many{n}.txt" for n in range(14)]
    for path in many:
        path.write_text("m")
    long = run(data, "rm", "--", *[str(path) for path in many])
    assert long.returncode == 1
    assert "14 path(s)" in long.stderr, long.stderr
    assert "... and 4 more" in long.stderr, long.stderr
    assert str(many[0].resolve()) in long.stderr
    assert str(many[13].resolve()) not in long.stderr, "the cap did not hold"

    # prune asks the same way, and names what it would drop.
    stale = room / "stale.txt"
    stale.write_text("s")
    run(data, "tag", "add", "--tag", "asked", "--", str(stale))
    stale.unlink()
    asked = run(data, "prune", str(room))
    assert asked.returncode == 1
    assert str(stale.resolve()) in asked.stderr, asked.stderr
    # And it says which tree, the way `prune --all` says which index. The
    # scoped form said only `prune:`, leaving the scope to be read off a path
    # list that stops at ten and then counts.
    assert str(room.resolve()) in asked.stderr.splitlines()[0], asked.stderr


def check_mv_and_cp_answer_in_one_shape(data, workspace):
    """--json mv and --json cp say where the file went, in every case.

    One verb answered in three shapes: `{from,to}` in a dry run, an array of
    rows in a real one, and `[]` when the file carried no tags -- so the one
    thing every consumer needs, the destination, was the one thing none of them
    could rely on, and the plain-text form printed it in all three. `rm` and
    `trash` have said `{"removed":[...]}` in both modes all along."""
    room = workspace / "oneshape"
    room.mkdir()
    tagged = room / "tagged.txt"
    bare = room / "bare.txt"
    tagged.write_text("t")
    bare.write_text("b")
    run(data, "tag", "add", "--tag", "shaped", "--", str(tagged))

    def answer(*args):
        got = run(data, "--json", *args)
        assert got.returncode == 0, got.stderr
        parsed = json.loads(got.stdout)
        assert set(parsed) == {"from", "to", "rows"}, parsed
        return parsed

    for verb in ("mv", "cp"):
        # The dry run and the real one describe the same write, rows included:
        # a plan whose shape differs from the result is a plan you cannot test
        # against. `rm --dry-run` has always listed the rows it would drop.
        landing = room / f"{verb}-tagged.txt"
        planned = answer("--dry-run", verb, str(tagged), str(landing))
        assert planned["from"] == str(tagged.resolve())
        assert planned["to"] == str(landing.resolve())
        assert [row["tags"] for row in planned["rows"]] == [["shaped"]]

        done = answer(verb, str(tagged), str(landing))
        assert done["to"] == str(landing.resolve())
        assert [row["path"] for row in done["rows"]] == [str(landing.resolve())]
        if verb == "mv":
            tagged = landing               # mv took it with it

    # An untagged file has no rows, and the destination is still the answer --
    # this was the case that returned `[]` and told the caller nothing at all.
    for verb in ("mv", "cp"):
        landing = room / f"{verb}-bare.txt"
        done = answer(verb, str(bare), str(landing))
        assert done["to"] == str(landing.resolve()), done
        assert done["rows"] == [], done
        if verb == "mv":
            bare = landing

    # And the answer only lands once the row that records it has: a JSON
    # document naming a destination is a claim the index has to back.
    assert run(data, "verify", str(room)).returncode == 0


def check_prune_keeps_what_it_cannot_see(data, workspace):
    """A path nobody may look at is not a path that is gone.

    `pathExists` was QFileInfo::exists, which cannot tell ENOENT from EACCES, so
    a directory somebody chmod'd shut -- an unmounted share, an unplugged drive,
    a folder mid-move -- read as deletion and `prune` dropped every row beneath
    it. On a filesystem that does not carry user.* xattrs the index is the only
    copy of those tags, and nothing, `reindex` included, could bring them back.
    git's is_missing_file_error() counts ENOENT and ENOTDIR and nothing else;
    TMSU's repair steps over a permission error and only forgets a file that is
    genuinely not there."""
    room = workspace / "unlookable"
    room.mkdir()
    locked = room / "locked"
    locked.mkdir()
    subject = locked / "l.txt"
    subject.write_text("l")
    run(data, "tag", "add", "--tag", "sealed", "--", str(subject))
    locked.chmod(0o000)
    try:
        # verify tells the two apart, because they want different repairs: one
        # wants prune, the other wants a chmod or a mount. Calling both
        # `missing` is what licensed prune to drop the row.
        seen = run(data, "verify", str(room))
        assert seen.returncode == 1, seen.stdout
        rows = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
        kinds = {row["path"]: row["problem"] for row in rows}
        assert kinds.get(str(subject.resolve())) == "unreadable", kinds
        assert "missing" not in kinds.values(), kinds

        # And prune steps over it rather than guessing -- and says so, because a
        # row it did not judge is one the person who asked should hear about.
        pruned = run(data, "--yes", "prune", str(room))
        assert pruned.returncode == 0, pruned.stderr
        assert str(subject.resolve()) not in pruned.stdout, "prune dropped a row it could not see"
        assert str(subject.resolve()) in pruned.stderr, pruned.stderr
    finally:
        locked.chmod(0o755)
    assert str(subject.resolve()) in run(data, "export").stdout, "the row went anyway"
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["sealed"]

    # A row whose file really is gone is still dropped. The fix is a narrower
    # question, not a refusal to answer one.
    subject.unlink()
    gone = run(data, "--yes", "prune", str(room))
    assert gone.returncode == 0, gone.stderr
    assert str(subject.resolve()) in gone.stdout, gone.stdout


def check_prune_keeps_a_file_the_owner_can_still_stat(data, workspace):
    """mode 000 on a file is Present, not stepped over.

    The owner can still stat a file with no permission bits set, so prune keeps
    the row and prints nothing about it. That is not a path the system will not
    describe -- a chmod'd directory on the walk is."""
    room = workspace / "unwritable-file"
    room.mkdir()
    subject = room / "u.txt"
    subject.write_text("u")
    resolved = str(subject.resolve())
    run(data, "tag", "add", "--tag", "modezero", "--", str(subject))
    subject.chmod(0o000)
    try:
        pruned = run(data, "--yes", "prune", "--all")
        assert pruned.returncode == 0, pruned.stderr
        assert resolved not in pruned.stderr, pruned.stderr
        assert resolved not in pruned.stdout.splitlines(), pruned.stdout
        assert resolved in run(data, "export").stdout, "prune dropped a Present row"
        rows = json.loads(run(data, "--json", "ls").stdout)
        assert resolved in {row["path"] for row in rows}, rows
        assert "modezero	1" in run(data, "tags").stdout
        assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["modezero"]
    finally:
        subject.chmod(0o644)


def check_verify_reports_one_cause_once(data, workspace):
    """One chmod is one problem, however many of verify's questions it answers.

    The index side reported `xattr-unreadable` and skipped the `seen` insert, so
    the disk walk found the same file and reported it again -- the same object
    twice under --json. An indexed shut directory came back twice under two
    different kinds, `xattr-unreadable` and `unreadable`, for that one reason."""
    room = workspace / "saidtwice"
    room.mkdir()
    sealed = room / "sealed.txt"
    sealed.write_text("s")
    run(data, "tag", "add", "--tag", "twice", "--", str(sealed))
    sealed.chmod(0o000)
    try:
        rows = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
        mine = [row for row in rows if row["path"] == str(sealed.resolve())]
        assert [row["problem"] for row in mine] == ["xattr-unreadable"], mine
    finally:
        sealed.chmod(0o644)

    shut = room / "shutdir"
    shut.mkdir()
    run(data, "tag", "add", "--tag", "twice", "--", str(shut))
    shut.chmod(0o000)
    try:
        rows = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
        mine = [row for row in rows if row["path"] == str(shut.resolve())]
        # The cause, not the consequence: `unreadable` also says the tree under
        # it went unchecked, which `xattr-unreadable` does not.
        assert [row["problem"] for row in mine] == ["unreadable"], mine
    finally:
        shut.chmod(0o755)


def check_reindex_settles_or_says_what_will(data, workspace):
    """`reindex && verify` has to reach a fixed point.

    An xattr set by hand that the tag rules reject left a state none of verify,
    reindex or prune could clear: verify called it `unindexed`, which is the
    kind that means "run reindex", and reindex printed the invalid tag and
    exited 0 having done nothing. A cron job alarmed forever with no command to
    act on."""
    room = workspace / "settling"
    room.mkdir()

    # What can be salvaged is salvaged. All-or-nothing is the right rule for a
    # list somebody typed and the wrong one for a value read off a file:
    # throwing `good` away along with `Bad Tag` left nothing to index at all.
    part = room / "part.txt"
    part.write_text("p")
    os.setxattr(part, "user.omafiles.tags", b"good,Bad Tag")
    rebuilt = run(data, "reindex", str(room))
    assert rebuilt.returncode == 0, rebuilt.stderr
    assert "Bad Tag" in rebuilt.stderr, rebuilt.stderr
    assert run(data, "tag", "ls", "--", str(part)).stdout.split() == ["good"]
    # The file's copy is cleaned with it, or verify goes on reporting the row it
    # just wrote as one the disk disagrees with.
    settled = run(data, "verify", str(room))
    assert settled.returncode == 0, settled.stdout
    assert os.getxattr(part, "user.omafiles.tags") == b"good"

    # Nothing salvageable, and no row to fall back on. reindex cannot invent a
    # row out of `Has Space!` and will not delete a value it did not write, so
    # it names the command that can -- and exits nonzero, because a run that
    # walked away from a path did not do what it was asked. `cp`, `chmod` and
    # `rm -f` all report one file they could not handle that way.
    bad = room / "bad.txt"
    bad.write_text("b")
    os.setxattr(bad, "user.omafiles.tags", b"Has Space!")
    for args in (("reindex", str(room)), ("reindex", "--from-xattr", str(room))):
        walked = run(data, *args)
        assert walked.returncode == 1, (args, walked.stderr)
        assert "tag clear" in walked.stderr, walked.stderr
    rows = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
    mine = [row for row in rows if row["path"] == str(bad.resolve())]
    # Not `unindexed`, which promises reindex will settle it.
    assert [row["problem"] for row in mine] == ["xattr-invalid"], mine
    assert "tag clear" in mine[0]["detail"], mine

    # And the command it named ends it, for good.
    assert run(data, "tag", "clear", "--", str(bad)).returncode == 0
    assert run(data, "reindex", str(room)).returncode == 0
    assert run(data, "verify", str(room)).returncode == 0

    # The indexed spelling still works the way it did: plain reindex writes the
    # row back over whatever the file says, which is the repair for every other
    # shape of this.
    os.setxattr(part, "user.omafiles.tags", b"Has Space!")
    assert run(data, "reindex", str(room)).returncode == 0
    assert os.getxattr(part, "user.omafiles.tags") == b"good"
    assert run(data, "verify", str(room)).returncode == 0

    # An indexed file whose own copy has grown a value the row was cleaned of.
    # Both spellings compared the row against the salvaged subset instead of
    # against what the file literally says, so `good` matched `good,Bad Tag`,
    # nothing was rewritten, and verify went on reporting `xattr-differs` for a
    # file reindex had just looked at.
    for spelling in (("reindex", str(room)), ("reindex", "--from-xattr", str(room))):
        os.setxattr(part, "user.omafiles.tags", b"good,Bad Tag")
        again = run(data, *spelling)
        assert again.returncode == 0, (spelling, again.stderr)
        assert os.getxattr(part, "user.omafiles.tags") == b"good", spelling
        assert run(data, "verify", str(room)).returncode == 0, spelling

    # Inside a batch the run still commits: a path reindex could not settle is a
    # diagnosis, not a failed write, and the rows that did land survive it. The
    # status is the line's own, though -- see
    # check_a_batch_carries_a_line_that_did_not_settle.
    os.setxattr(bad, "user.omafiles.tags", b"Has Space!")
    fresh = room / "fresh.txt"
    fresh.write_text("f")
    batched = run(data, "batch", "-",
                  stdin=f"tag add --tag batched -- {fresh}\nreindex {room}\n")
    assert batched.returncode == 1, batched.stderr
    assert "tag clear" in batched.stderr, batched.stderr
    assert run(data, "tag", "ls", "--", str(fresh)).stdout.split() == ["batched"]


def check_reindex_quiet_keeps_the_answer(data, workspace):
    """--quiet is "No stderr except errors", and stdout is not stderr.

    reindex was the one verb that gagged its own answer: `--quiet reindex`
    printed nothing while writing rows, while `--json --quiet` printed its array
    all along -- so the flag meant one thing with --json and another without.
    prune, import and batch all answer whatever --quiet says."""
    room = workspace / "quietly"
    room.mkdir()
    subject = room / "q.txt"
    subject.write_text("q")
    os.setxattr(subject, "user.omafiles.tags", b"quietly")
    hushed = run(data, "--quiet", "reindex", str(room))
    assert hushed.returncode == 0, hushed.stderr
    assert hushed.stdout.splitlines() == [str(subject.resolve())], hushed.stdout

    # And it does silence the warnings, which are the stderr the flag is about.
    noisy = room / "noisy.txt"
    noisy.write_text("n")
    os.setxattr(noisy, "user.omafiles.tags", b"kept,Bad Tag")
    quiet = run(data, "--quiet", "reindex", str(room))
    assert quiet.stdout.splitlines() == [str(noisy.resolve())], quiet.stdout
    assert quiet.stderr == "", quiet.stderr


def check_each_tag_subcommand_takes_its_own_flags(data, workspace):
    """Each `tag` subcommand has its own flags, not the whole family's.

    The six shared one parse call that allowed --tag and --from, so every one of
    them accepted its siblings' flags and then ignored them. `tag clear --tag
    drop` was the worst of it: somebody typed a restricting flag and got an
    unrestricted destructive write, silently and at exit 0."""
    room = workspace / "tagflags"
    room.mkdir()
    subject = room / "s.txt"
    subject.write_text("s")
    other = room / "o.txt"
    other.write_text("o")

    # The matrix the bug lived in, as omafiles.usage.kdl draws it: -t/--tag on
    # add and rm, --from on cp, and no flags at all on ls, clear and merge.
    for sub, flag in (("add", "--from"),
                      ("rm", "--from"),
                      ("ls", "--tag"), ("ls", "-t"), ("ls", "--from"),
                      ("clear", "--tag"), ("clear", "-t"), ("clear", "--from"),
                      ("cp", "--tag"), ("cp", "-t"),
                      ("merge", "--tag"), ("merge", "--from"),
                      ("rename", "--tag"), ("rename", "--from")):
        stray = run(data, "tag", sub, flag, "value", "--", str(subject))
        assert stray.returncode == 1, f"tag {sub} accepted {flag}"
        assert f"unexpected {flag}" in stray.stderr, stray.stderr

    # The write that motivated all of it: both tags went, and the index row with
    # them, for a command that had named one of them.
    run(data, "tag", "add", "--tag", "drop", "--tag", "keep", "--", str(subject))
    refused = run(data, "tag", "clear", "--tag", "drop", "--", str(subject))
    assert refused.returncode == 1, refused.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["drop", "keep"]

    # And `tag ls --tag base` printed every tag on the path while appearing to
    # honour a filter, which is a wrong answer rather than a refused one.
    filtered = run(data, "tag", "ls", "--tag", "drop", "--", str(subject))
    assert filtered.returncode == 1
    assert filtered.stdout == "", filtered.stdout

    # --from eats the next word, so on a verb without it, it ate the flag that
    # followed: this reported `--tag is required` to somebody who had typed it.
    swallowed = run(data, "tag", "add", "--from", "--tag", "work", "--", str(subject))
    assert swallowed.returncode == 1
    assert "unexpected --from" in swallowed.stderr, swallowed.stderr

    # It ate the `--` just as happily, which breaks the one promise the README
    # makes about odd names: "Flags come first and paths after --, so
    # -weird.pdf is a path and not a flag." `tag clear --from -- -leading.txt`
    # came back as `unknown flag: -leading.txt`, and the --tag spelling cleared
    # the file at exit 0.
    weird = room / "-leading.txt"
    weird.write_text("w")
    assert run(data, "tag", "add", "--tag", "odd", "--", "-leading.txt",
               cwd=room).returncode == 0
    for flag in ("--from", "--tag"):
        eaten = run(data, "tag", "clear", flag, "--", "-leading.txt", cwd=room)
        assert eaten.returncode == 1, flag
        assert f"unexpected {flag}" in eaten.stderr, eaten.stderr
    assert run(data, "tag", "ls", "--", "-leading.txt", cwd=room).stdout.split() == ["odd"]

    # Each still has the flag it does have, and the ones with none still run.
    assert run(data, "tag", "rm", "--tag", "drop", "--", str(subject)).returncode == 0
    assert run(data, "tag", "cp", "--from", str(subject), "--", str(other)).returncode == 0
    assert run(data, "tag", "ls", "--", str(other)).stdout.split() == ["keep"]
    assert run(data, "tag", "clear", "--", str(other)).returncode == 0
    assert run(data, "tag", "merge", "keep", "kept").returncode == 0
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["kept"]

    # A wrong subcommand is still reported as one. Judging the flags first would
    # answer `tag nonsense --tag x` with a complaint about the flag.
    bogus = run(data, "tag", "nonsense", "--tag", "x")
    assert bogus.returncode == 1
    assert "unknown subcommand" in bogus.stderr, bogus.stderr


def check_mv_carries_every_row_it_can(data, workspace):
    """Every row under a folder follows it, and the preview promises no more.

    `followMove` rebuilt each row by stat'ing where the file had landed, so a
    row it could not stat -- one under a directory nobody may open, one pointing
    at a dangling link -- was taken out of the index and never put back: exit 2,
    the tags gone from the row, and the file left carrying its own copy for
    `verify` to call `unindexed`. The dry run rewrites the path and asks nothing,
    so it promised rows the real run then did not deliver."""
    room = workspace / "partialmove"
    (room / "tree/shut").mkdir(parents=True)
    (room / "dest").mkdir()
    here = str(room.resolve())
    good = [room / "tree/a.txt", room / "tree/d.txt"]
    buried = room / "tree/shut/buried.txt"
    # Rows a stat would refuse, for both of the reasons it can: a dangling link
    # answers ENOENT and a file under a shut directory EACCES.
    bad = [room / "tree/b.txt", room / "tree/c.txt"]
    # A sibling whose name starts with the folder's, to prove the prefix match
    # is on a path separator and not on the text.
    decoy = room / "treehouse.txt"
    for f in good + bad + [buried, decoy]:
        f.write_text("x")
    tagged = run(data, "tag", "add", "--tag", "carried", "--",
                 *map(str, good + bad + [buried, decoy]))
    assert tagged.returncode == 0, tagged.stderr
    for f in bad:
        f.unlink()
        os.symlink("/nonexistent", f)            # a row pointing at a dangling link
    (room / "tree/shut").chmod(0o000)

    try:
        preview = run(data, "--json", "--dry-run", "mv",
                      str(room / "tree"), str(room / "dest/tree"))
        assert preview.returncode == 0, preview.stderr
        moved = run(data, "--json", "mv", str(room / "tree"), str(room / "dest/tree"))
        # The rename happened and every row followed it, so there is nothing
        # for the status to be short of.
        assert moved.returncode == 0, (moved.returncode, moved.stderr)
        # The equality is the property, not the count: a preview that promises
        # rows the real run does not deliver is a preview nothing can be tested
        # against, and the kdl says the two print the same object.
        assert json.loads(moved.stdout) == json.loads(preview.stdout), \
            (preview.stdout, moved.stdout)
        carried = sorted(row["path"] for row in json.loads(moved.stdout)["rows"])
        assert carried == sorted(
            [f"{here}/dest/tree/{name}" for name in ("a.txt", "b.txt", "c.txt", "d.txt")]
            + [f"{here}/dest/tree/shut/buried.txt"]), carried
    finally:
        (room / "dest/tree/shut").chmod(0o755)

    rows = run(data, "export").stdout
    for name in ("a.txt", "b.txt", "c.txt", "d.txt", "shut/buried.txt"):
        assert f"{here}/dest/tree/{name}" in rows, (name, rows)
        assert f"{here}/tree/{name}" not in rows, (name, rows)
    # The decoy kept its own row, untouched.
    assert f"{here}/treehouse.txt" in rows, rows

    # Which is the whole point: the row under the shut directory is in the index
    # under its new name, so the file is not one the index has never heard of.
    # The dangling links are `missing` at the new path, as they were at the old.
    kinds = {problem["path"]: problem["problem"] for problem
             in json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]}
    assert f"{here}/dest/tree/shut/buried.txt" not in kinds, kinds
    assert kinds.get(f"{here}/dest/tree/b.txt") == "missing", kinds
    assert kinds.get(f"{here}/dest/tree/c.txt") == "missing", kinds
    assert run(data, "tag", "ls", "--",
               str(room / "dest/tree/a.txt")).stdout.split() == ["carried"]


def check_an_option_value_is_not_a_global(data, workspace):
    """The word after a flag that takes one is its value, not a global.

    The globals were stripped in a pass before the flag/value binding, so a
    value spelled like one disappeared and the flag ate the word after it:
    `ls --ext -j sub` turned on --json, `--ext` swallowed the [dir], and the
    program answered a different question with no diagnostic at all. POSIX
    Utility Syntax Guideline 6 says that token is the argument, taken
    literally."""
    room = workspace / "optvalues"
    (room / "sub").mkdir(parents=True)
    # A file whose extension really is `-j`, so the answer shows both halves at
    # once: the value bound literally, and the [dir] still there to restrict it.
    inside = room / "sub/inside.-j"
    outside = room / "outside.-j"
    for f in (inside, outside):
        f.write_text("x")
    run(data, "tag", "add", "--tag", "optval", "--", str(inside), str(outside))

    bound = run(data, "ls", "--ext", "-j", str(room / "sub"))
    assert bound.returncode == 0, bound.stderr
    # Under the bug this was `[]`: JSON, from a query with no directory at all.
    assert bound.stdout.splitlines() == [str(inside.resolve())], bound.stdout

    # And the value is judged as the value it is, rather than quietly becoming a
    # flag and leaving the next word to be eaten in its place.
    literal = run(data, "ls", "--tag", "-j", str(room / "sub"))
    assert literal.returncode == 1, literal.stdout
    assert "invalid tag: -j" in literal.stderr, literal.stderr

    # `--` is the one escape hatch a value that looks like a flag has, so a flag
    # that eats it takes the escape hatch with it. `-y` here is a real file.
    odd = room / "-y"
    odd.write_text("y")
    assert run(data, "tag", "add", "--tag", "odd", "--", "-y", cwd=room).returncode == 0
    target = room / "b.md"
    target.write_text("b")
    copied = run(data, "tag", "cp", "--from", "-y", "--", "b.md", cwd=room)
    # Under the bug: `-y` became --yes, `--from` ate the `--`, and the run died
    # with `path missing: --`.
    assert copied.returncode == 0, copied.stderr
    assert run(data, "tag", "ls", "--", str(target)).stdout.split() == ["odd"]

    # The globals still work on both sides of the verb, and out of the
    # environment, which is what the pass they were stripped in was for.
    assert json.loads(run(data, "--json", "ls", "--tag", "optval").stdout)
    assert json.loads(run(data, "ls", "--tag", "optval", "--json").stdout)
    assert json.loads(run(data, "ls", "--tag", "optval",
                          extra_env={"OMAFILES_JSON": "1"}).stdout)


def check_an_undated_row_is_not_stamped_by_nothing(data, workspace):
    """A write that changes no tag does not stamp a row that has no stamp.

    The guard that keeps `recent` in order carried the old stamp forward, and on
    a row written before the field existed that value is zero -- which `stamped`
    read as "nothing to carry" and replaced with now. So `tag rm --tag` naming a
    tag the file never had sent it to the top of `recent`, on the migration path
    where it is hardest to notice. The README says of those rows that they are
    stamped the next time their tags change."""
    room = workspace / "undated"
    room.mkdir()
    subject = room / "g.txt"
    subject.write_text("g")
    source = room / "src.txt"
    source.write_text("s")
    run(data, "tag", "add", "--tag", "legacy", "--", str(source))

    # A row as an index written before `tagged` existed holds it: no such key.
    index = data / "omafiles/index.jsonl"
    with index.open("a") as handle:
        handle.write(json.dumps({"path": str(subject.resolve()),
                                 "tags": ["legacy"]}) + "\n")

    def row():
        return json.loads(run(data, "--json", "json", "--", str(subject)).stdout)

    assert "tagged" not in row(), row()
    for changes_nothing in (("tag", "rm", "--tag", "notpresent", "--", str(subject)),
                            ("tag", "add", "--tag", "legacy", "--", str(subject)),
                            ("tag", "cp", "--from", str(source), "--", str(subject))):
        done = run(data, *changes_nothing)
        assert done.returncode == 0, (changes_nothing, done.stderr)
        assert "tagged" not in row(), (changes_nothing, row())
    # And it stays out of `recent`, which is the answer the stamp is for.
    assert str(subject.resolve()) not in run(data, "recent", "5").stdout

    # A write that does change a tag stamps it, which is the promise the rest of
    # this rests on.
    assert run(data, "tag", "add", "--tag", "now", "--", str(subject)).returncode == 0
    assert row()["tagged"] > 0, row()
    # Asked wide enough to hold the whole index: the point is that the row is
    # datable at all now, not where it sorts among rows stamped the same second.
    assert str(subject.resolve()) in run(data, "recent", "500").stdout


def check_a_refused_xattr_is_not_a_success(data, workspace):
    """A file's copy the filesystem refused is an error, and reindex says so.

    `syncXattr` dropped the failure into a warning and `flushPending` ignored
    it, so `reindex` printed the path, exited 0 and left the exact disagreement
    `verify` fails on -- `reindex && verify` with no fixed point, which is the
    thing the code beside that exit was written to prevent. `--quiet` hid the
    warning outright, so the run was silent on both streams."""
    room = workspace / "refused"
    room.mkdir()
    subject = room / "s.txt"
    subject.write_text("s")
    run(data, "tag", "add", "--tag", "want", "--", str(subject))
    # The state reindex exists to repair: the row says one thing, the file's own
    # copy another, and the file may be read but not written.
    os.setxattr(subject, "user.omafiles.tags", b"stale")
    subject.chmod(0o444)
    try:
        assert run(data, "verify", str(room)).returncode == 1

        walked = run(data, "reindex", str(room))
        assert walked.returncode == 1, walked.stderr
        assert str(subject.resolve()) in walked.stderr, walked.stderr
        # The index is the source of truth and the xattr is written where the
        # filesystem allows it, so the row still lands -- the failed write is
        # reported, not rolled back.
        assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["want"]

        # `--quiet` is documented "No stderr except errors". A write the
        # filesystem refused is an error, not a note.
        hushed = run(data, "--quiet", "reindex", str(room))
        assert hushed.returncode == 1, hushed.stdout
        assert str(subject.resolve()) in hushed.stderr, hushed.stderr
    finally:
        subject.chmod(0o644)

    # And once the permission is what it has to be, the pair converges -- which
    # is the only thing the nonzero exit was ever asking for.
    assert run(data, "reindex", str(room)).returncode == 0
    assert run(data, "verify", str(room)).returncode == 0
    assert os.getxattr(subject, "user.omafiles.tags") == b"want"


def check_a_dry_run_refuses_what_the_real_run_refuses(data, workspace):
    """--dry-run is "print the write, do not commit", not "print a write".

    Three refusals were preconditions the real run met on its way to the
    system call, and the dry-run branch returned before any of them: `rm`, `cp`
    and `mv` each printed a write the real run turns down flat. `rm` was the
    worst of the three, because it printed the index rows it would drop -- so a
    dry run read as "these tags are about to go" for a command that would not
    have touched them. None of the three needs the work done to know the
    answer."""
    room = workspace / "dry-refusals"
    room.mkdir()
    folder = room / "folder"
    folder.mkdir()
    inside = folder / "in.txt"
    inside.write_text("i")
    run(data, "tag", "add", "--tag", "dry", "--", str(folder), str(inside))

    # `rm` unlinks and cannot unlink a directory. --yes on the real run,
    # because the suite has no terminal and the prompt would otherwise be the
    # thing that failed.
    dry_rm = run(data, "--dry-run", "rm", str(folder))
    real_rm = run(data, "--yes", "rm", str(folder))
    assert dry_rm.returncode == real_rm.returncode == 1, (dry_rm.stderr, real_rm.stderr)
    assert "is a directory" in dry_rm.stderr, dry_rm.stderr
    assert "is a directory" in real_rm.stderr, real_rm.stderr
    assert str(inside.resolve()) not in dry_rm.stdout, "the dry run named rows it would drop"
    assert folder.is_dir() and inside.exists()
    assert str(inside.resolve()) in run(data, "export").stdout, "a row went anyway"

    # `cp` copies bytes and a directory has none.
    copy = room / "copy"
    dry_cp = run(data, "--dry-run", "cp", str(folder), str(copy))
    real_cp = run(data, "cp", str(folder), str(copy))
    assert dry_cp.returncode == real_cp.returncode == 1, (dry_cp.stderr, real_cp.stderr)
    assert "is a directory" in dry_cp.stderr, dry_cp.stderr
    assert dry_cp.stdout.strip() == "", dry_cp.stdout
    assert not copy.exists()

    # A folder is renamed and never copied, so it cannot cross a filesystem --
    # `QFile::rename` falls back to a copy and a delete for a regular file
    # only. The kernel gave that refusal, which meant only the real run ever
    # met it; the device of the source against the device of the
    # destination's parent is the same comparison `rename(2)` makes, and it
    # can be made without moving anything.
    shm = Path("/dev/shm")
    if shm.is_dir() and os.stat(shm).st_dev != os.stat(room).st_dev:
        across = shm / "omafiles-cross-device"
        dry_mv = run(data, "--dry-run", "mv", str(folder), str(across))
        real_mv = run(data, "mv", str(folder), str(across))
        assert dry_mv.returncode == real_mv.returncode == 1, (dry_mv.stderr, real_mv.stderr)
        assert "cross" in dry_mv.stderr, dry_mv.stderr
        assert "cross" in real_mv.stderr, real_mv.stderr
        assert dry_mv.stdout.strip() == "", dry_mv.stdout
        assert not across.exists() and folder.is_dir()
        # A single file does cross, in both runs: the fallback covers it, so
        # refusing one would be the dry run inventing a refusal of its own.
        crossing = room / "crossing.txt"
        crossing.write_text("c")
        run(data, "tag", "add", "--tag", "dry", "--", str(crossing))
        landing = shm / "omafiles-crossed.txt"
        try:
            dry_file = run(data, "--dry-run", "mv", str(crossing), str(landing))
            assert dry_file.returncode == 0, dry_file.stderr
            assert not landing.exists(), "the dry run moved it"
            real_file = run(data, "mv", str(crossing), str(landing))
            assert real_file.returncode == 0, real_file.stderr
            assert landing.exists() and not crossing.exists()
        finally:
            landing.unlink(missing_ok=True)

    # And the writes it would really make are still printed. A dry run that
    # refused everything would be no more honest than one that promised
    # everything.
    plain = room / "plain.txt"
    plain.write_text("p")
    run(data, "tag", "add", "--tag", "dry", "--", str(plain))
    fine = run(data, "--dry-run", "rm", str(plain))
    assert fine.returncode == 0, fine.stderr
    assert str(plain.resolve()) in fine.stdout, fine.stdout
    assert plain.exists(), "the dry run removed it"
    # A folder is what `trash` does take, so the refusal is `rm`'s alone.
    assert run(data, "--dry-run", "trash", str(folder)).returncode == 0


def check_from_xattr_says_when_it_declines(data, workspace):
    """--from-xattr exits nonzero on the path it walked away from.

    A row whose file has lost its xattr -- a copy without --preserve=xattr, or
    a filesystem that never carried it -- is the one case where the flag
    declines to do what it says. Declining is right: an empty value almost
    always means the value was dropped, not that somebody cleared the tags. But
    it said so on stderr and exited 0, so `reindex --from-xattr && verify` was a
    check with no fixed point -- the verify could only report `xattr-differs`,
    and nothing in the pair said which command would end it."""
    room = workspace / "declining"
    room.mkdir()
    subject = room / "e.txt"
    subject.write_text("e")
    run(data, "tag", "add", "--tag", "alpha", "--", str(subject))
    try:
        os.removexattr(subject, "user.omafiles.tags")
    except OSError:
        return                                   # no xattr here, nothing to say

    declined = run(data, "reindex", "--from-xattr", str(room))
    assert declined.returncode == 1, declined.stderr
    assert str(subject.resolve()) in declined.stderr, declined.stderr
    # The row is kept, which is the whole reason it declined.
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["alpha"]
    assert run(data, "verify", str(room)).returncode == 1

    # And the message names the command that converges, which plain `reindex`
    # does: it writes the row back over the file's copy.
    assert "reindex" in declined.stderr, declined.stderr
    assert run(data, "reindex", str(room)).returncode == 0
    assert run(data, "verify", str(room)).returncode == 0
    assert os.getxattr(subject, "user.omafiles.tags") == b"alpha"

    # The same rule for the other value it cannot take at its word. No row can
    # be invented out of `Has Space!` and a rebuild will not delete a value it
    # did not write, so the flag keeps the row here too -- and that is the same
    # path walked away from, so it is the same nonzero exit. The two used to
    # disagree only in that both exited 0.
    os.setxattr(subject, "user.omafiles.tags", b"Has Space!")
    unusable = run(data, "reindex", "--from-xattr", str(room))
    assert unusable.returncode == 1, unusable.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["alpha"]
    assert run(data, "reindex", str(room)).returncode == 0
    assert os.getxattr(subject, "user.omafiles.tags") == b"alpha"


def check_a_shut_directory_is_not_an_empty_index(data, workspace):
    """The three read verbs say what they could not look at, and list the rest.

    `pathExists` was QFileInfo::exists in `ls`, `tags` and `recent` too, so a
    directory somebody chmod'd shut -- an unmounted share, an unplugged drive --
    read as deletion and the rows under it left the answer with nothing on
    either stream: `ls --tag work` exited 0 printing nothing about a file that
    was sitting right there, `tags` reported a vocabulary short of the tags only
    those files carry, and `recent` dropped the thing you tagged a minute ago
    off the top of the list. `ls --untagged` already warned about exactly this,
    so one `ls` invocation was honest and the other was not. Skipping the row is
    right -- nothing can list what the system will not describe -- but it is
    said out loud, on stderr, where `--quiet` can take it away."""
    room = workspace / "unlistable"
    room.mkdir()
    locked = room / "locked"
    locked.mkdir()
    subject = locked / "s.txt"
    subject.write_text("s")
    run(data, "tag", "add", "--tag", "shuttag", "--", str(subject))
    resolved = str(subject.resolve())

    locked.chmod(0o000)
    try:
        # ls: the row is not listed, and the reason is -- and the status says
        # the list is short, because `--quiet` is allowed to take the note away
        # and something has to be left saying so. See
        # check_an_incomplete_answer_is_not_a_success.
        listed = run(data, "ls", "--tag", "shuttag")
        assert listed.returncode == 1, listed.stderr
        assert resolved not in listed.stdout, "ls listed a path it cannot stat"
        assert resolved in listed.stderr, "ls dropped the row in silence"
        assert "Permission denied" in listed.stderr, listed.stderr

        # Through warn(), like every other note here, so --quiet means it.
        hushed = run(data, "--quiet", "ls", "--tag", "shuttag")
        assert hushed.stderr == "", hushed.stderr
        assert hushed.returncode == 1, "--quiet hid the status as well as the note"

        # tags: the vocabulary is the same one row short either way, but the
        # verb people read to find out what tags they have says which row.
        vocab = run(data, "tags", str(room))
        assert vocab.returncode == 1, vocab.stderr
        assert "shuttag" not in vocab.stdout, "tags counted a path it cannot stat"
        assert resolved in vocab.stderr, "tags miscounted the vocabulary in silence"
        assert run(data, "--quiet", "tags", str(room)).stderr == ""

        # recent: a list read by what is at the top of it is where a silently
        # dropped row hides best.
        newest = run(data, "recent", "500")
        assert newest.returncode == 1, newest.stderr
        assert resolved not in newest.stdout, "recent listed a path it cannot stat"
        assert resolved in newest.stderr, "recent dropped the row in silence"
        assert run(data, "--quiet", "recent", "500").stderr == ""
    finally:
        locked.chmod(0o755)

    # And the chmod undone, all three answer again -- and exit 0 again: the
    # status is about the answer being short, not about the tree.
    whole = run(data, "ls", "--tag", "shuttag")
    assert whole.returncode == 0 and resolved in whole.stdout, whole.stderr
    assert run(data, "tags", str(room)).returncode == 0
    assert "shuttag\t1" in run(data, "tags", str(room)).stdout
    assert run(data, "recent", "500").returncode == 0
    assert resolved in run(data, "recent", "500").stdout

    # A row whose file really is gone is still dropped, and silently: "it is
    # gone" is not news, and a note about every stale row in the index would
    # bury the one that matters.
    subject.unlink()
    gone = run(data, "ls", "--tag", "shuttag")
    assert gone.stdout.strip() == "", gone.stdout
    assert gone.stderr == "", gone.stderr
    assert gone.returncode == 0, "a row that is gone made the answer look short"


def check_from_xattr_writes_nothing_it_just_read(data, workspace):
    """--from-xattr does not write the value back onto the file it read it from.

    The skip-if-equal check lived only on the plain `reindex` path, so
    `--from-xattr` rewrote the xattr with the value it had just read out of that
    same xattr. On a writable file that is a ctime bump for nothing. On a file
    the filesystem will not take a write for, that pointless write failed, and
    the verb exited 1 reporting it had walked away from a path -- over a tree
    its own `verify` calls clean, which is the same "no fixed point" the exit
    code beside it exists to prevent."""
    room = workspace / "already-said"
    room.mkdir()
    subject = room / "ro.txt"
    subject.write_text("r")
    run(data, "tag", "add", "--tag", "idxtag", "--", str(subject))
    try:
        os.setxattr(subject, "user.omafiles.tags", b"xattrtag")
    except OSError:
        return                                   # no xattr here, nothing to say

    # The row and the file disagree, so the row is rewritten -- but the file
    # already says what --from-xattr is about to make the row say.
    subject.chmod(0o444)
    try:
        rebuilt = run(data, "reindex", "--from-xattr", str(room))
        assert rebuilt.returncode == 0, rebuilt.stderr
        assert "xattr" not in rebuilt.stderr, rebuilt.stderr
        assert str(subject.resolve()) in rebuilt.stdout, rebuilt.stdout
    finally:
        subject.chmod(0o644)
    # It walked away from nothing, and verify agrees the tree is settled.
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["xattrtag"]
    assert run(data, "verify", str(room)).returncode == 0

    # The same write, on a file that would have taken it: the row moves and the
    # file is not touched at all, which is what the read-only case is a loud
    # version of.
    os.setxattr(subject, "user.omafiles.tags", b"turned")
    before = os.stat(subject).st_ctime_ns
    moved = run(data, "reindex", "--from-xattr", str(room))
    assert moved.returncode == 0, moved.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["turned"]
    assert os.stat(subject).st_ctime_ns == before, "the file was rewritten with its own value"

    # A file whose copy really does differ from the value the row will carry is
    # still written: the skip is an equality, not a refusal to mirror.
    os.setxattr(subject, "user.omafiles.tags", b"kept,Bad Tag")
    salvaged = run(data, "reindex", "--from-xattr", str(room))
    assert salvaged.returncode == 0, salvaged.stderr
    assert os.getxattr(subject, "user.omafiles.tags") == b"kept"
    assert run(data, "verify", str(room)).returncode == 0


def check_a_batch_prints_nothing_until_it_commits(data, workspace):
    """A reindex line inside a batch does not announce rows a later line unwrites.

    `batch` refuses `ls` because "a batch prints nothing until it commits", and
    then broke the rule itself: `reindex` wrote its rebuilt paths to stdout the
    moment it had them, before the commit that belongs to batch. A later failing
    line rolled the store back and left those paths on stdout naming rows that
    were never written -- the index and every xattr byte-identical to before.
    `prune` already defers its dropped list this way; `reindex` was left out."""
    room = workspace / "batched-reindex"
    room.mkdir()
    subject = room / "a.txt"
    subject.write_text("a")
    doomed = room / "b.txt"
    doomed.write_text("b")
    run(data, "tag", "add", "--tag", "keep", "--", str(subject))
    try:
        os.setxattr(subject, "user.omafiles.tags", b"other")
    except OSError:
        return                                   # no xattr here, nothing to say

    rolled = run(data, "batch", "-", stdin=(
        f"reindex {room}\n"
        f'tag add --tag "BAD TAG" -- {doomed}\n'
    ))
    assert rolled.returncode != 0, rolled.stdout
    assert rolled.stdout == "", "batch printed a row it then rolled back"
    assert "line 2" in rolled.stderr, rolled.stderr
    # And the rollback is real, which is what makes the printed path a phantom:
    # both copies are exactly as they were found.
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["keep"]
    assert os.getxattr(subject, "user.omafiles.tags") == b"other"

    # A batch that commits still does the work; only the announcement was ever
    # in question, and no other write verb announces one inside a batch either.
    ran = run(data, "batch", "-", stdin=f"reindex {room}\n")
    assert ran.returncode == 0, ran.stderr
    assert os.getxattr(subject, "user.omafiles.tags") == b"keep"
    assert run(data, "verify", str(room)).returncode == 0

    # On its own the verb still says what it touched: the list was deferred, not
    # deleted.
    os.setxattr(subject, "user.omafiles.tags", b"drifted")
    alone = run(data, "reindex", str(room))
    assert alone.returncode == 0, alone.stderr
    assert str(subject.resolve()) in alone.stdout.splitlines(), alone.stdout



def check_a_precondition_is_settled_before_the_prompt(data, workspace):
    """A prompt must not name a path the run then refuses.

    `rm g1.txt somedir g3.txt` over a terminal counted three paths, asked, and
    on `y` unlinked `g1.txt` and refused `somedir`: consent given for three
    removals, one carried out, two accounted for, and the first one already
    gone. Whether a name is a directory is a pure precondition -- it is one or
    it is not, whatever the run does next -- and the check sat inside the
    per-path loop, one branch past the comment that says a dry run naming tags
    as about to go, for a command that will not go, is worse than no dry run at
    all. `path missing` was hoisted above the prompt long ago, so the
    pre-validation pass existed and this was simply not in it.

    `trash` has the same shape with `gio`: whether it is on PATH takes no work
    to find out, and `--dry-run trash G` enumerated every row under `G` and
    exited 0 for a command whose real run says `gio is not installed`."""
    room = workspace / "precondition"
    room.mkdir()
    first = room / "g1.txt"
    blocker = room / "somedir"
    last = room / "g3.txt"
    first.write_text("1")
    blocker.mkdir()
    last.write_text("3")
    run(data, "tag", "add", "--tag", "asked-first", "--", str(first), str(last))

    status, seen = run_pty(data, "rm", str(first), str(blocker), str(last))
    assert status == 1, (status, seen)
    assert "is a directory" in seen, seen
    # The question is never put, the way it is never put for a missing path.
    assert "Continue?" not in seen, seen
    assert first.exists() and last.exists(), seen
    rows = run(data, "export").stdout
    assert str(first.resolve()) in rows and str(last.resolve()) in rows

    # Every offending operand, not the first: coreutils `rm` and `chmod` report
    # each one they cannot handle rather than stopping at one.
    second = room / "otherdir"
    second.mkdir()
    both = run(data, "--yes", "rm", "--", str(blocker), str(second))
    assert both.returncode == 1
    assert str(blocker.resolve()) in both.stderr, both.stderr
    assert str(second.resolve()) in both.stderr, "only the first directory was named"

    # And a dry run makes the same refusal, because nothing about it waits on
    # the work. It used to list every row under the directory as one it was
    # about to drop.
    dry = run(data, "--dry-run", "rm", "--", str(first), str(blocker), str(last))
    assert dry.returncode == 1, dry.stdout
    assert dry.stdout.strip() == "", dry.stdout

    # trash: gio is a lookup on PATH, so both modes answer the same way.
    nowhere = workspace / "no-gio-here"
    nowhere.mkdir(exist_ok=True)
    tree = room / "G"
    (tree / "s").mkdir(parents=True)
    (tree / "x").write_text("x")
    (tree / "s/y").write_text("y")
    run(data, "tag", "add", "--tag", "asked-first", "--",
        str(tree), str(tree / "x"), str(tree / "s/y"))
    empty_path = {"PATH": str(nowhere)}
    real = run(data, "--yes", "trash", str(tree), extra_env=empty_path)
    assert real.returncode == 1 and "gio is not installed" in real.stderr, real.stderr
    for shape in (("--dry-run",), ("--json", "--dry-run")):
        blind = run(data, *shape, "trash", str(tree), extra_env=empty_path)
        assert blind.returncode == real.returncode, (shape, blind.returncode)
        assert "gio is not installed" in blind.stderr, (shape, blind.stderr)
        assert blind.stdout.strip() == "", (shape, blind.stdout)
    # Nor is it asked about, for the same reason.
    status, seen = run_pty(data, "trash", str(tree), extra_env=empty_path)
    assert status == 1 and "Continue?" not in seen, seen
    assert tree.exists()


def check_every_operand_is_tried(data, workspace):
    """rm and trash do not abandon the tail of the list in silence.

    `rm z1.txt noperm/z2.txt z3.txt` removed `z1.txt`, named the failure and
    returned -- so `z3.txt` was neither removed nor mentioned: three asked for,
    one done, two accounted for. Measured on the same fixture, `/usr/bin/rm
    y1.txt cdir y3.txt` unlinks both files it can and reports the one it cannot,
    exit 1; `chmod` and `chown` report every operand they walked away from too.
    The refusals this verb decides for itself are settled before the prompt, so
    what is left in the loop is only what the system itself answers."""
    room = workspace / "everyoperand"
    sealed = room / "sealed"
    sealed.mkdir(parents=True)
    first = room / "z1.txt"
    middle = sealed / "z2.txt"
    last = room / "z3.txt"
    for f in (first, middle, last):
        f.write_text("z")
    run(data, "tag", "add", "--tag", "operand", "--",
        str(first), str(middle), str(last))

    sealed.chmod(0o555)                      # unlink under it is refused
    try:
        tried = run(data, "--yes", "rm", "--", str(first), str(middle), str(last))
        assert tried.returncode == 1, tried.stderr
        assert str(middle.resolve()) in tried.stderr, tried.stderr
        assert not first.exists(), "the operand before the failure was not removed"
        assert not last.exists(), "the operand after the failure was abandoned"
        assert middle.exists()
        # And what went took its row with it, which is the other half: an index
        # keeping rows for files that are not there is the state `prune` exists
        # to clean up after.
        rows = run(data, "export").stdout
        assert str(first.resolve()) not in rows, "a deleted file kept its index row"
        assert str(last.resolve()) not in rows, "a deleted file kept its index row"
        assert str(middle.resolve()) in rows, "a row went for a file that did not"
        assert str(last.resolve()) in tried.stdout, tried.stdout
    finally:
        sealed.chmod(0o755)


def check_a_dry_run_needs_somewhere_to_land(data, workspace):
    """mv and cp check the destination's parent before they print the move.

    `--dry-run mv s.txt nodir/s.txt` printed the arrow and exited 0, and
    `--json --dry-run` went further and enumerated the index rows landing under
    a name nothing can create; the real run answers `No such file or directory`
    and exits 1. A `stat` on the parent settles it, and both verbs already make
    the already-exists and cross-filesystem refusals in the dry run, so the
    pattern was there. Same measurement from coreutils: `mv s.txt nodir/s.txt`
    is `cannot move ... No such file or directory`, exit 1."""
    room = workspace / "nowheretoland"
    room.mkdir()
    subject = room / "s.txt"
    subject.write_text("s")
    run(data, "tag", "add", "--tag", "landing", "--", str(subject))
    nowhere = room / "nodir/s.txt"

    for verb in ("mv", "cp"):
        real = run(data, verb, str(subject), str(nowhere))
        assert real.returncode == 1, (verb, real.stdout)
        for shape in ((), ("--json",)):
            blind = run(data, *shape, "--dry-run", verb, str(subject), str(nowhere))
            assert blind.returncode == real.returncode, (verb, shape, blind.stdout)
            assert blind.stdout.strip() == "", (verb, shape, blind.stdout)
            assert "No such file or directory" in blind.stderr, (verb, shape, blind.stderr)
        assert subject.exists()

    # A parent that is there but is not a directory cannot hold it either, and
    # `rename(2)` says ENOTDIR rather than ENOENT.
    wall = room / "wall.txt"
    wall.write_text("w")
    blocked = run(data, "--dry-run", "mv", str(subject), str(wall / "s.txt"))
    assert blocked.returncode == 1, blocked.stdout
    assert "Not a directory" in blocked.stderr, blocked.stderr

    # And with a parent that is there, the dry run names where it would land --
    # the line the real run prints.
    (room / "dest").mkdir()
    fine = run(data, "--dry-run", "mv", str(subject), str(room / "dest/s.txt"))
    assert fine.returncode == 0, fine.stderr
    assert fine.stdout.splitlines() == [str((room / "dest/s.txt").resolve())], fine.stdout


def check_open_is_all_or_nothing(data, workspace):
    """open resolves every operand before it launches any of them.

    `open a.txt nosuch b.txt` launched `a.txt`, refused at `nosuch` and never
    mentioned `b.txt` -- half a set of windows, the tail abandoned, and a
    partial JSON array to say which half. `rm` and `trash` resolve every operand
    up front and do nothing at all if one is missing; `open` has the least undo
    of the three, since a launched program cannot be un-launched, so it keeps
    that rule rather than a third one."""
    room = workspace / "opening"
    room.mkdir()
    first = room / "a.txt"
    last = room / "b.txt"
    first.write_text("a")
    last.write_text("b")

    # A private PATH holding one fake opener, so nothing on this machine's
    # desktop is ever started and the launch is a line in a log.
    fakebin = room / "bin"
    fakebin.mkdir()
    log = room / "opened.log"
    opener = fakebin / "xdg-open"
    opener.write_text(f'#!/bin/sh\necho "$1" >> {log}\n')
    opener.chmod(0o755)
    only_fake = {"PATH": str(fakebin)}

    missed = run(data, "open", str(first), str(room / "nosuch"), str(last),
                 extra_env=only_fake)
    assert missed.returncode == 2, missed.stderr
    assert not log.exists(), "open launched the head of a list it was going to refuse"
    # Nothing half-said on stdout either: there is no half to report.
    assert missed.stdout.strip() == "", missed.stdout
    empty = run(data, "--json", "open", str(first), str(room / "nosuch"),
                extra_env=only_fake)
    assert empty.returncode == 2 and empty.stdout.strip() == "", empty.stdout

    # And with every operand there, every operand is launched.
    both = run(data, "open", str(first), str(last), extra_env=only_fake)
    assert both.returncode == 0, both.stderr
    for _ in range(50):
        if log.exists() and len(log.read_text().splitlines()) == 2:
            break
        time.sleep(0.05)
    # Sorted: `startDetached` gives no order, and the point here is that both
    # were launched rather than which went first.
    assert sorted(log.read_text().split()) \
        == sorted([str(first.resolve()), str(last.resolve())]), log.read_text()


def check_a_dir_it_may_not_look_into_is_not_a_dir_that_is_gone(data, workspace):
    """The exit table's own worked example was wrong for a `[dir]`.

    A row the system will not describe is exit 1 -- `ls` and `tags` skip it,
    `verify` calls it `unreadable` -- and every operand of `rm`, `mv` and the
    tag verbs answers 1 the same way. The directory the query is scoped to did
    not: `optionalDir` hard-coded the status where `resolveAll` reads it off the
    `PathState`, so `omafiles ls outer/inner` under a chmod'd `outer` printed
    `Permission denied` and exited 2, the code for a path that is not there. The
    dump `import` and `batch` read, and the parent `mv` and `cp` are asked to
    land a file in, were the same shape: an open or a `QFileInfo::exists` that
    is false for both reasons, answering as though it were only the one."""
    room = workspace / "shut-dir-operand"
    outer = room / "outer"
    inner = outer / "inner"
    inner.mkdir(parents=True)
    subject = inner / "deep.txt"
    subject.write_text("d")
    source = room / "src.txt"
    source.write_text("s")
    assert run(data, "tag", "add", "--tag", "shutdir", "--", str(subject)).returncode == 0

    # Every verb that takes a `[dir]`, both verbs that read a file of their own,
    # and both verbs that have to be handed somewhere to land a file.
    scoped = (("ls",), ("ls", "--untagged"), ("tags",), ("verify",), ("reindex",),
              ("--yes", "prune"))
    outer.chmod(0o000)
    try:
        for args in scoped:
            shut = run(data, *args, str(inner))
            assert shut.returncode == 1, (args, shut.returncode, shut.stderr)
            assert "Permission denied" in shut.stderr, (args, shut.stderr)
            assert "path missing" not in shut.stderr, (args, shut.stderr)
        for verb in ("import", "batch"):
            shut = run(data, verb, str(inner / "dump.json"))
            assert shut.returncode == 1, (verb, shut.returncode, shut.stderr)
            assert f"{verb}: cannot read" in shut.stderr, (verb, shut.stderr)
        for verb in ("mv", "cp"):
            shut = run(data, verb, str(source), str(inner / "new.txt"))
            assert shut.returncode == 1, (verb, shut.returncode, shut.stderr)
            # The system's own reason, and the component it is about: `exists()`
            # is false for a shut directory too, and `No such file or directory`
            # about one sends the reader after the wrong repair.
            assert "Permission denied" in shut.stderr, (verb, shut.stderr)
            assert "No such file or directory" not in shut.stderr, (verb, shut.stderr)
            assert source.exists()
    finally:
        outer.chmod(0o755)

    # A `[dir]` that really is not there keeps 2: the code has to stay worth
    # reading, and this is the half that says which of the two happened.
    gone = room / "never-here"
    for args in scoped:
        absent = run(data, *args, str(gone))
        assert absent.returncode == 2, (args, absent.returncode, absent.stderr)
        assert f"path missing: {gone}" in absent.stderr, (args, absent.stderr)
    for verb in ("import", "batch"):
        absent = run(data, verb, str(gone / "dump.json"))
        assert absent.returncode == 2, (verb, absent.returncode, absent.stderr)
    for verb in ("mv", "cp"):
        absent = run(data, verb, str(source), str(gone / "new.txt"))
        assert absent.returncode == 1, (verb, absent.returncode, absent.stderr)
        assert "No such file or directory" in absent.stderr, (verb, absent.stderr)

    # And a directory readable but not searchable: `--untagged` can list the
    # names inside it and cannot stat one of them, which it used to drop in
    # silence and report as a whole answer, exit 0.
    listable = room / "listable"
    listable.mkdir()
    (listable / "l.txt").write_text("l")
    listable.chmod(0o444)
    try:
        blind = run(data, "ls", "--untagged", str(listable))
        assert blind.returncode == 1, (blind.returncode, blind.stdout)
        assert blind.stdout.strip() == "", blind.stdout
        assert str((listable / "l.txt").resolve()) in blind.stderr, blind.stderr
        # The status survives --quiet; the note is what --quiet takes.
        hushed = run(data, "--quiet", "ls", "--untagged", str(listable))
        assert hushed.returncode == 1 and hushed.stderr == "", hushed.stderr
    finally:
        listable.chmod(0o755)



def check_a_scope_nobody_may_open_is_not_an_empty_answer(data, workspace):
    """An indexed `[dir]` nobody may open is not an empty successful answer.

    `optionalDir` only asks whether the name is a directory; `stat` still answers
    for a chmod 000 tree the process owns, so `ls`, `tags` and `prune` could
    iterate the index, find no rows under it, print nothing and exit 0 -- as if
    the folder were simply empty. `ls --untagged` already caught this through
    `tagWalk`; these three do not walk the disk."""
    shut = workspace / "z"
    shut.mkdir()
    shut.chmod(0o000)
    try:
        for args in (("ls", str(shut)),
                     ("tags", str(shut)),
                     ("--yes", "prune", str(shut))):
            refused = run(data, *args)
            assert refused.returncode == 1, (args, refused.returncode, refused.stderr)
            assert refused.stdout.strip() == "", (args, refused.stdout)
            assert "Permission denied" in refused.stderr or "unreadable" in refused.stderr, \
                (args, refused.stderr)
            hushed = run(data, "--quiet", *args)
            assert hushed.returncode == 1, (args, hushed.returncode)
            assert hushed.stderr == "", (args, hushed.stderr)

        shaped = run(data, "--json", "ls", str(shut))
        assert shaped.returncode == 1, shaped.stderr
        assert json.loads(shaped.stdout) == [], shaped.stdout

        pruned = run(data, "--json", "--yes", "prune", str(shut))
        assert pruned.returncode == 1, pruned.stderr
        assert json.loads(pruned.stdout) == {"dropped": []}, pruned.stdout

        untagged = run(data, "ls", "--untagged", str(shut))
        assert untagged.returncode == 1, (untagged.returncode, untagged.stderr)
    finally:
        shut.chmod(0o755)

    gone = workspace / "never-was"
    for args in (("ls", str(gone)),
                 ("tags", str(gone)),
                 ("--yes", "prune", str(gone))):
        absent = run(data, *args)
        assert absent.returncode == 2, (args, absent.returncode, absent.stderr)
        assert f"path missing: {gone}" in absent.stderr, (args, absent.stderr)



def check_a_missing_path_is_settled_before_the_index(data, workspace):
    """A path that is not there is exit 2 before the index is opened.

    Verbs that resolve a named operand on disk -- rm, mv, cp, tag add -- settle
    `path missing` before openSession, so a malformed jsonl cannot turn a
    missing path into exit 3. A verb that only reads the index, and __complete
    loading tags, still name the store or have no opinion."""
    room = workspace / "index-order"
    room.mkdir()
    present = room / "here.txt"
    present.write_text("h")
    missing = room / "nowhere.txt"
    index = data / "omafiles/index.jsonl"

    assert run(data, "tag", "add", "--tag", "work", "--", str(present)).returncode == 0
    index.write_text("THIS IS NOT JSON\n")

    bad = run(data, "ls")
    assert bad.returncode == 3, (bad.returncode, bad.stderr)
    assert "store: malformed jsonl" in bad.stderr, bad.stderr

    for args in (("--yes", "rm", "--", str(missing)),
                 ("tag", "add", "--tag", "work", "--", str(missing)),
                 ("mv", str(missing), str(room / "dst.txt")),
                 ("cp", str(missing), str(room / "copy.txt"))):
        refused = run(data, *args)
        assert refused.returncode == 2, (args, refused.returncode, refused.stderr)
        assert f"path missing: {missing}" in refused.stderr, (args, refused.stderr)
        assert "malformed jsonl" not in refused.stderr, (args, refused.stderr)

    blocked = run(data, "--yes", "rm", "--", str(present))
    assert blocked.returncode == 3, (blocked.returncode, blocked.stderr)
    assert "malformed jsonl" in blocked.stderr, blocked.stderr
    assert present.exists(), "rm acted on a file though the store would not load"

    complete = run(data, "__complete", "--", "ls", "--tag", "")
    assert complete.returncode == 1, (complete.returncode, complete.stdout, complete.stderr)
    assert complete.stdout == "", complete.stdout
    assert "malformed" not in complete.stdout, complete.stdout

    still = run(data, "ls")
    assert still.returncode == 3, still.stderr

    # Later checks share this data directory; delete the bad file and tag again.
    index.unlink()
    assert run(data, "tag", "add", "--tag", "work", "--", str(present)).returncode == 0


def check_quiet_takes_the_note_beside_a_success(data, workspace):
    """--quiet is "No stderr except errors", and this was not an error.

    A filesystem that refuses to mirror the tags onto the file is `tag add`'s
    documented exit 0 -- the row is the record -- and the refusal still went out
    through `fail`, which `--quiet` cannot empty. So `--quiet tag add` on a
    chmod 444 file printed to stderr and exited 0, which is the one combination
    the flag promises never to produce. Where the same refusal is why the run
    exits 1 -- `reindex`, and `tag clear` on a path with no row -- it is the
    reason for the status, so it stays an error and `--quiet` keeps it."""
    room = workspace / "quiet-mirror"
    room.mkdir()
    probe = room / "probe.txt"
    probe.write_text("p")
    try:
        os.setxattr(probe, "user.omafiles.tags", b"probe")
    except OSError:
        return                                   # no user.* here, nothing to refuse

    # Exit 0, so the refusal is a note: `tag add`, `tag clear` over a row, and
    # `import`, each on a file the filesystem will not take a write for.
    def sealed(name):
        subject = room / name
        subject.write_text("s")
        return subject

    # Fresh subjects per run: a clear that lands takes the row with it, and the
    # next run would find the rowless case, which is the other rule entirely.
    def notes(suffix):
        added = sealed(f"add{suffix}.txt")
        added.chmod(0o444)
        imported = sealed(f"imp{suffix}.txt")
        dump = room / f"dump{suffix}.json"
        dump.write_text(json.dumps([{"path": str(imported.resolve()),
                                     "tags": ["imported"]}]))
        imported.chmod(0o444)
        cleared = sealed(f"clear{suffix}.txt")
        assert run(data, "tag", "add", "--tag", "rowed", "--",
                   str(cleared)).returncode == 0
        cleared.chmod(0o444)
        return ((("tag", "add", "--tag", "q", "--", str(added)), added),
                (("import", str(dump)), imported),
                (("tag", "clear", "--", str(cleared)), cleared))

    for globals_ in ([], ["--quiet"]):
        for args, subject in notes("-quiet" if globals_ else "-loud"):
            try:
                ran = run(data, *globals_, *args)
                assert ran.returncode == 0, (args, globals_, ran.returncode, ran.stderr)
                if globals_:
                    assert ran.stderr == "", (args, ran.stderr)
                else:
                    assert "Permission denied" in ran.stderr, (args, ran.stderr)
            finally:
                subject.chmod(0o644)

    # The other half, unchanged: where the refused mirror is the reason for the
    # 1, --quiet still names the path. `tag clear` on a path with no row, and
    # `reindex` over a file whose copy has to be rewritten.
    rowless = sealed("rowless.txt")
    os.setxattr(rowless, "user.omafiles.tags", b"stuck")
    rowless.chmod(0o444)
    walked = room / "walked"
    walked.mkdir()
    stale = walked / "w.txt"
    stale.write_text("w")
    assert run(data, "tag", "add", "--tag", "kept", "--", str(stale)).returncode == 0
    os.setxattr(stale, "user.omafiles.tags", b"other")
    stale.chmod(0o444)
    try:
        for args, named in ((("tag", "clear", "--", str(rowless)), rowless),
                            (("reindex", str(walked)), stale)):
            for globals_ in ([], ["--quiet"]):
                errored = run(data, *globals_, *args)
                assert errored.returncode == 1, (args, globals_, errored.returncode)
                assert str(named.resolve()) in errored.stderr, (args, globals_,
                                                                errored.stderr)
    finally:
        rowless.chmod(0o644)
        stale.chmod(0o644)

    # And the rule is written down where the CLI reference lives.
    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    assert "--quiet" in reference and "the row is the record" in reference


def check_a_source_it_cannot_read_through_is_missing(data, workspace):
    """A dangling link is the link to the verbs that act on a name, and the
    target to the verbs that read through it -- and the message says which.

    `resolveNamed` is happy with a link that points nowhere, because for `rm`,
    `trash` and `mv` a dangling link is a path that is there. `cp` reads the
    bytes at the far end and every `tag` verb writes the tags there -- a `user.*`
    xattr cannot sit on a link at all -- so for them the far end is the whole of
    what they wanted, and it is not there: exit 2, the same one a name that was
    never there gets. What was wrong was the sentence. `path missing: dang` is
    false about a link `ls -l` lists and `rm` removes, and it says nothing about
    the target, which is the end that has to be created or repointed."""
    room = workspace / "unreadable-source"
    room.mkdir()
    dangling = room / "dang"
    target = room / "never-existed"
    os.symlink(str(target), dangling)

    def reads_through(result):
        assert result.returncode == 2, (result.returncode, result.stderr)
        assert f"{dangling} is a symlink to {target}, which is not there" \
            in result.stderr, result.stderr
        assert "path missing" not in result.stderr, result.stderr

    copied = run(data, "cp", str(dangling), str(room / "out.txt"))
    reads_through(copied)
    assert not (room / "out.txt").exists()

    # Every verb that reads through the link, measured rather than assumed --
    # the tag verbs, the two that print a row, `open`, and `[dir]`.
    for verb in (("tag", "ls", "--"), ("tag", "add", "--tag", "x", "--"),
                 ("tag", "rm", "--tag", "x", "--"), ("tag", "clear", "--"),
                 ("tag", "cp", "--from", str(dangling), "--"),
                 ("json",), ("open",), ("has", "--tag", "x", "--"), ("ls",)):
        reads_through(run(data, *verb, str(dangling)))

    # A name that really was never there keeps the sentence it always had: the
    # two are the same status and not the same diagnosis.
    absent = run(data, "tag", "ls", "--", str(target))
    assert absent.returncode == 2, absent.stderr
    assert f"path missing: {target}" in absent.stderr, absent.stderr

    # `mv`, `rm` and `trash` act on the name, and the name is there.
    landed = room / "moved-link"
    moved = run(data, "mv", str(dangling), str(landed))
    assert moved.returncode == 0, moved.stderr
    assert os.path.islink(landed) and not dangling.exists()
    swept = run(data, "--yes", "rm", str(landed))
    assert swept.returncode == 0, swept.stderr
    assert not os.path.islink(landed)

    # And the rule is written down in the CLI reference; README points there.
    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    assert "symlink is the link it is to the verbs that act on a name" in reference
    readme_text = (ROOT / "README.md").read_text()
    assert "symlink" in " ".join(readme_text.split())
    assert "docs/cli.md" in readme_text


def check_an_incomplete_answer_is_not_a_success(data, workspace):
    """A list one row short is not the answer that was asked for.

    `ls`, `tags` and `recent` skip a row the system will not describe, say so on
    stderr and used to exit 0 -- so `--quiet`, which is allowed to take the note
    away, took the only sign with it, and a script got a truncated list with a
    success status. `ls --untagged` over a directory nobody may open was the
    same. /usr/bin/ls exits 2 and find exits 1 in that situation; neither of
    them reports an empty directory, and this program's own `finished` already
    exits 1 rather than let a truncated write look like a whole answer. The
    exit table has no fourth code, so it is the 1 that already means "it ran and
    the answer is no"."""
    room = workspace / "incomplete"
    locked = room / "locked"
    locked.mkdir(parents=True)
    subject = locked / "i.txt"
    subject.write_text("i")
    run(data, "tag", "add", "--tag", "incomplete-tag", "--", str(subject))

    locked.chmod(0o000)
    try:
        for args in (("ls", "--tag", "incomplete-tag"),
                     ("tags", str(room)),
                     ("recent", "500"),
                     ("ls", "--untagged", str(room))):
            short = run(data, *args)
            assert short.returncode == 1, (args, short.returncode, short.stderr)
            # The status survives --quiet; the note is what --quiet takes.
            hushed = run(data, "--quiet", *args)
            assert hushed.returncode == 1, (args, hushed.returncode)
            assert hushed.stderr == "", (args, hushed.stderr)
            # --json is the same answer in another shape, so it is the same
            # status: the array is short either way.
            shaped = run(data, "--json", *args)
            assert shaped.returncode == 1, (args, shaped.returncode)
    finally:
        locked.chmod(0o755)

    # The chmod undone, all four are whole again and exit 0. The status is
    # about the answer being short, not about the tree.
    for args in (("ls", "--tag", "incomplete-tag"),
                 ("tags", str(room)),
                 ("recent", "500"),
                 ("ls", "--untagged", str(room))):
        assert run(data, *args).returncode == 0, args

    # A row whose file really is gone is not an incomplete read: "it is gone" is
    # a complete answer, and `prune` is the verb for it.
    subject.unlink()
    assert run(data, "ls", "--tag", "incomplete-tag").returncode == 0

    # And a row `--since` cannot judge is a narrower answer rather than a
    # shorter one: the rows are there, the index simply cannot date them.
    index = data / "omafiles/index.jsonl"
    undated = room / "undated.txt"
    undated.write_text("u")
    with index.open("a") as handle:
        handle.write(json.dumps({"path": str(undated.resolve()),
                                 "tags": ["incomplete-tag"]}) + "\n")
    judged = run(data, "ls", "--tag", "incomplete-tag", "--since", "1h")
    assert judged.returncode == 0, judged.stderr
    assert "predate the tagged field" in judged.stderr, judged.stderr


def check_a_usage_line_carries_the_usage(data, workspace):
    """`Usage: omafiles prune` left out the usage.

    The line named the verb and dropped the `[dir]` and `[--all]` that the body
    two lines under it and docs/cli.md both carry, so the one line a reader
    scans first was the one line with nothing in it. Every verb had it; `prune`
    is just where it shows worst, because both of its arguments are optional."""
    expected = {
        "prune": "Usage: omafiles prune [dir] [--all]",
        "verify": "Usage: omafiles verify [dir]",
        "mv": "Usage: omafiles mv <old> <new>",
        "recent": "Usage: omafiles recent [n]",
        "has": "Usage: omafiles has --tag <tag> -- <path>",
        "reindex": "Usage: omafiles reindex [--from-xattr] [dir]",
        "completions": "Usage: omafiles completions <bash|zsh|fish>",
        # `export` takes no arguments, so its first gap is its description and
        # not its shape. A rule that split on the gap alone printed the prose.
        "export": "Usage: omafiles export",
    }
    for verb, line in expected.items():
        helped = run(data, verb, "--help")
        assert helped.returncode == 0, (verb, helped.stderr)
        assert helped.stdout.splitlines()[0] == line, (verb, helped.stdout)

    # Two words, and the alias the body does not start with.
    for verb, line in (("tag add", "Usage: omafiles tag add --tag <tag>... -- <path>..."),
                       ("tag cp", "Usage: omafiles tag cp --from <path> -- <path>..."),
                       ("tag rename", "Usage: omafiles tag rename <from> <to>")):
        helped = run(data, *verb.split(), "--help")
        assert helped.stdout.splitlines()[0] == line, (verb, helped.stdout)

    # `ls` wraps its flags onto a second line, so the synopsis is gathered from
    # every line left of the description column and not just the first.
    listing = run(data, "ls", "--help").stdout.splitlines()[0]
    assert "[--untagged]" in listing and "[--since <duration>] [dir]" in listing, listing

    # And `tag` alone names all six subcommands, so it carries no one shape:
    # six synopses run together is not a usage line.
    assert run(data, "tag", "--help").stdout.splitlines()[0] == "Usage: omafiles tag"


def check_the_divergences_are_written_down(data, workspace):
    """Three deliberate divergences, each measured here and documented there.

    Each of these is a decision rather than a bug, and each was reachable from
    the command line with nothing in README.md, docs/cli.md or --help to say it
    was meant. A divergence nobody wrote down is indistinguishable from one
    nobody noticed."""
    # Flattened: prose wraps, and a phrase split over two lines is still the
    # phrase. The point of these is that the sentence exists, not where it sits.
    readme = " ".join((ROOT / "README.md").read_text().split())
    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    room = workspace / "divergences"
    room.mkdir()

    # 1. mv and cp refuse to clobber where coreutils overwrites.
    source = room / "src.txt"
    taken = room / "taken.txt"
    source.write_text("s")
    taken.write_text("t")
    for verb in ("mv", "cp"):
        refused = run(data, verb, str(source), str(taken))
        assert refused.returncode == 1 and "already exists" in refused.stderr, refused.stderr
        assert taken.read_text() == "t", "the destination was clobbered"
        assert "already exists" in run(data, "--dry-run", verb,
                                       str(source), str(taken)).stderr
        helped = " ".join(run(data, verb, "--help").stdout.split())
        assert "over what is already there" in helped, verb
    assert "write over something already at the destination" in readme
    assert reference.count("refused rather than overwritten") == 2, "mv or cp says nothing"

    # 2. A dry run does not predict a write the filesystem will refuse. The only
    # way to find out whether a `user.*` write is taken is to make it, and every
    # proxy is wrong in both directions -- so `reindex --dry-run` prints the
    # path and exits 0 where the real run exits 1, and that is the contract.
    stubborn = room / "ro.txt"
    stubborn.write_text("r")
    assert run(data, "tag", "add", "--tag", "sticky", "--", str(stubborn)).returncode == 0
    try:
        os.removexattr(stubborn, "user.omafiles.tags")
    except OSError:
        return                               # no user.* here; nothing to measure
    stubborn.chmod(0o444)
    try:
        blind = run(data, "--dry-run", "reindex", str(room))
        assert blind.returncode == 0, blind.stderr
        assert str(stubborn.resolve()) in blind.stdout, blind.stdout
        real = run(data, "reindex", str(room))
        assert real.returncode == 1, real.stderr
        assert "Permission denied" in real.stderr, real.stderr
        assert "can only be found out by making the write" in reference

        # 3. tag add exits 0 for that same refused write and reindex exits 1,
        # because they promise different things: the row is the record, and a
        # filesystem with no user.* namespace must not make every `tag add` on
        # it fail -- while `reindex` promises the tree it walked agrees.
        added = run(data, "tag", "add", "--tag", "extra", "--", str(stubborn))
        assert added.returncode == 0, added.stderr
        assert "Permission denied" in added.stderr, added.stderr
        assert run(data, "tag", "ls", "--", str(stubborn)).stdout.split() \
            == ["extra", "sticky"], "the row did not land"
        assert "the run still exits 0" in reference
    finally:
        stubborn.chmod(0o644)

    # 4. verify calls that `xattr-differs`, whose two documented repairs are not
    # both available here: plain `reindex` cannot settle a file whose xattr is
    # readable but not writable, and the kind's documentation now says so.
    pair = room / "pair.txt"
    pair.write_text("p")
    run(data, "tag", "add", "--tag", "one", "--", str(pair))
    pair.chmod(0o444)
    try:
        run(data, "tag", "add", "--tag", "two", "--", str(pair))
        seen = run(data, "verify", str(room))
        assert seen.returncode == 1
        assert "xattr-differs" in seen.stdout, seen.stdout
        assert run(data, "reindex", str(room)).returncode == 1, "plain reindex settled it"
        assert run(data, "reindex", "--from-xattr", str(room)).returncode == 0
        assert run(data, "verify", str(room)).returncode == 0, "--from-xattr did not settle it"
        assert "readable but not writable" in reference, "xattr-differs is unqualified"
    finally:
        pair.chmod(0o644)



def check_a_set_is_removed_whole_or_not_at_all(data, workspace):
    """One operand rm and trash cannot take refuses the whole list.

    This is a deliberate divergence and the point of pinning it is that it is
    deliberate. Measured on the same fixture, `/usr/bin/rm c1.txt cdir c3.txt`
    unlinks c1 and c3 and reports only `cdir`, exit 1. These two refuse before
    anything goes, because the question they ask is about the set: you consented
    to three removals, and carrying out two of them because the third could not
    be done is worse than carrying out none and saying why. It is also the rule
    `path missing` has kept all along, so the two foreseeable failures now agree
    rather than one refusing everything and the other doing half.

    The split matters as much as the rule. A failure the checks cannot foresee
    -- a permission denied at unlink time -- is coreutils' case and is handled
    coreutils' way, in check_every_operand_is_tried: every remaining operand is
    tried, each failure is named, and what went takes its row with it."""
    room = workspace / "wholeset"
    room.mkdir()
    first = room / "y1.txt"
    blocker = room / "dir"
    last = room / "y3.txt"
    first.write_text("1")
    blocker.mkdir()
    last.write_text("3")
    run(data, "tag", "add", "--tag", "whole", "--", str(first), str(last))

    # rm: a directory in the middle. Nothing is unlinked, on either side of it.
    refused = run(data, "--yes", "rm", "--", str(first), str(blocker), str(last))
    assert refused.returncode == 1, refused.stderr
    assert "is a directory" in refused.stderr, refused.stderr
    assert first.exists(), "the operand before the refusal was removed anyway"
    assert last.exists(), "the operand after the refusal was removed anyway"
    # And no row moved either: a refused run leaves the index exactly as found.
    rows = run(data, "export").stdout
    assert str(first.resolve()) in rows and str(last.resolve()) in rows
    assert refused.stdout.strip() == "", refused.stdout

    # trash takes a directory, so its foreseeable failures are the other two: a
    # path that is not there, and a gio that is not installed. Both refuse the
    # whole list the same way.
    missing = run(data, "--yes", "trash", "--",
                  str(first), str(room / "nosuch"), str(last))
    assert missing.returncode == 2, missing.stderr
    assert first.exists() and last.exists(), "trash acted on part of a refused list"

    nowhere = workspace / "no-gio-at-all"
    nowhere.mkdir(exist_ok=True)
    ungio = run(data, "--yes", "trash", "--", str(first), str(blocker), str(last),
                extra_env={"PATH": str(nowhere)})
    assert ungio.returncode == 1, ungio.stderr
    assert "gio is not installed" in ungio.stderr, ungio.stderr
    # Once, and before the list is walked. Left in the per-path loop the answer
    # is the same three times over -- the same refusal, re-derived per operand,
    # which is what "settled once, before anything is trashed" rules out.
    assert ungio.stderr.count("gio is not installed") == 1, ungio.stderr
    assert first.exists() and last.exists() and blocker.exists()
    assert str(first.resolve()) in run(data, "export").stdout

    # With the blocker out of the list, the whole set goes.
    swept = run(data, "--yes", "rm", "--", str(first), str(last))
    assert swept.returncode == 0, swept.stderr
    assert not first.exists() and not last.exists()

    # And it is written down, in the same voice mv and cp got for their own
    # divergence -- an undocumented divergence is indistinguishable from one
    # nobody noticed. Flattened, because prose wraps.
    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    assert reference.count("refuses the whole run rather than doing part of it") == 2, \
        "rm or trash says nothing about refusing the whole list"
    assert "parts company with coreutils `rm`" in reference, \
        "the divergence is documented without naming what it diverges from"
    assert "refuse the whole list" in " ".join(run(data, "rm", "--help").stdout.split())


def check_a_link_crosses_a_filesystem_as_a_link(data, workspace):
    """mv carries the link across a filesystem, never the bytes at the far end.

    A `QFile::copy` fallback for EXDEV reads *through* a symlink: it lands a
    regular file holding the target's bytes with the named link gone, it has
    nothing to read for a link pointing nowhere and blames the source for it,
    and `QFileInfo::isDir` calls a link to a directory a folder and refuses it.
    Measured on these three fixtures, `/usr/bin/mv` moves each across and each
    arrives a link. `readlink` + `symlink` + `unlink` needs no target at all."""
    shm = Path("/dev/shm")
    room = workspace / "crossing-links"
    room.mkdir()
    if not shm.is_dir() or os.stat(shm).st_dev == os.stat(room).st_dev:
        return                                   # one filesystem; nothing to cross

    # Named for this run: /dev/shm is shared with everything else on the box.
    across = Path(tempfile.mkdtemp(prefix="omafiles-links-", dir=str(shm)))
    try:
        target = room / "t.txt"
        target.write_text("TARGET")
        run(data, "tag", "add", "--tag", "crossed", "--", str(target))

        # A link to a file. The link moves; the target stays where it is and
        # keeps its row, and nothing is duplicated at the destination.
        good = room / "flink"
        good.symlink_to(target)
        moved = run(data, "mv", str(good), str(across / "flink"))
        assert moved.returncode == 0, moved.stderr
        landed = across / "flink"
        assert landed.is_symlink(), "mv copied through the link instead of moving it"
        assert os.readlink(landed) == str(target)
        assert target.read_text() == "TARGET" and not good.is_symlink()
        assert run(data, "tag", "ls", "--", str(target)).stdout.split() == ["crossed"]

        # A link pointing nowhere. It is a path that is there, so it moves, and
        # the failure it used to report named the source rather than the link.
        broken = room / "dlink"
        broken.symlink_to(room / "never-existed")
        gone = run(data, "mv", str(broken), str(across / "dlink"))
        assert gone.returncode == 0, gone.stderr
        assert os.path.islink(across / "dlink"), gone.stderr
        assert not broken.is_symlink(), "the source was left in place"

        # A link to a directory is a link, not a folder. The folder refusal is
        # about the tree a folder holds, and a link holds none of it.
        tree = room / "tree"
        tree.mkdir()
        (tree / "inner.txt").write_text("i")
        run(data, "tag", "add", "--tag", "crossed", "--", str(tree / "inner.txt"))
        dirlink = room / "dirlink"
        dirlink.symlink_to(tree)
        walked = run(data, "mv", str(dirlink), str(across / "dirlink"))
        assert walked.returncode == 0, walked.stderr
        assert os.path.islink(across / "dirlink"), walked.stderr
        assert "cross" not in walked.stderr, walked.stderr
        # The tree itself did not move, and neither did the row inside it.
        assert (tree / "inner.txt").is_file()
        assert str((tree / "inner.txt").resolve()) in run(data, "export").stdout
        # A real folder is still refused: that divergence is unchanged.
        refused = run(data, "mv", str(tree), str(across / "tree"))
        assert refused.returncode == 1 and "cross" in refused.stderr, refused.stderr

        # A relative link crosses with the target it was written with. Resolving
        # it against the link's own directory would land a different link.
        relative = room / "rlink"
        os.symlink("../elsewhere/t.txt", relative)
        assert run(data, "mv", str(relative), str(across / "rlink")).returncode == 0
        assert os.readlink(across / "rlink") == "../elsewhere/t.txt"

        # Index, disk and xattr still agree on both sides of the move.
        assert run(data, "verify", str(room)).returncode == 0
        assert run(data, "verify", str(across)).returncode == 0

        # What a *file* loses by crossing is a shortfall against coreutils `mv`,
        # not a divergence -- so it is measured here and written down there. An
        # undocumented shortfall is indistinguishable from one nobody noticed.
        plain = room / "plain.txt"
        plain.write_text("body")
        run(data, "tag", "add", "--tag", "crossed", "--", str(plain))
        os.utime(plain, (946684800, 946684800))          # 2000-01-01
        foreign = True
        try:
            os.setxattr(plain, "user.foo", b"bar")
        except OSError:
            foreign = False                              # no user.* here
        was = os.stat(plain).st_mtime
        arrived = across / "plain.txt"
        assert run(data, "mv", str(plain), str(arrived)).returncode == 0
        assert arrived.read_text() == "body"
        assert run(data, "tag", "ls", "--", str(arrived)).stdout.split() == ["crossed"]
        assert os.stat(arrived).st_mtime != was, "the documented mtime loss is not real"
        if foreign:
            assert "user.foo" not in os.listxattr(arrived), \
                "the documented xattr loss is not real"
        reference = " ".join((ROOT / "docs/cli.md").read_text().split())
        assert "it does not carry the modification time or any other `user.*` xattr" \
            in reference, "mv says nothing about what a crossing copy drops"
        assert "A symlink is not a folder" in reference, \
            "mv says nothing about a link crossing as a link"
        assert run(data, "verify", str(across)).returncode == 0
    finally:
        shutil.rmtree(across, ignore_errors=True)


def check_every_operand_it_refuses_is_named(data, workspace):
    """rm, trash and open name every operand they cannot take, not the first.

    A resolver that returns at the first failure prints `no1` for `rm a.txt no1
    no2` and never mentions `no2`, and prints only `nosuch` for `rm dirA
    nosuch` -- while the is-a-directory check names both of `rm dirA dirB`.
    Measured on the same fixtures, `/usr/bin/rm a.txt no1 no2` reports both it
    cannot remove and `/usr/bin/rm dirA nosuch` reports both operands, exit 1.

    Only the report is coreutils'. The refusal is the documented divergence and
    is checked here too: nothing is acted on, and the whole list is refused."""
    room = workspace / "namethemall"
    room.mkdir()
    keeper = room / "a.txt"
    keeper.write_text("a")
    run(data, "tag", "add", "--tag", "named", "--", str(keeper))
    first = room / "no1"
    second = room / "no2"

    for verb in (("--yes", "rm"), ("--yes", "trash")):
        refused = run(data, *verb, "--", str(keeper), str(first), str(second))
        assert refused.returncode == 2, (verb, refused.stderr)
        for absent in (first, second):
            assert f"path missing: {absent}" in refused.stderr, (verb, refused.stderr)
        # And the set is still refused whole: that rule is deliberate.
        assert keeper.exists(), (verb, "an operand of a refused list was acted on")
        assert str(keeper.resolve()) in run(data, "export").stdout

    # A private PATH holding one fake opener, so nothing on this desktop is ever
    # started -- `open` refuses before it launches, and this pins that too.
    fakebin = room / "bin"
    fakebin.mkdir()
    log = room / "opened.log"
    opener = fakebin / "xdg-open"
    opener.write_text(f'#!/bin/sh\necho "$1" >> {log}\n')
    opener.chmod(0o755)
    unopened = run(data, "open", str(keeper), str(first), str(second),
                   extra_env={"PATH": str(fakebin)})
    assert unopened.returncode == 2, unopened.stderr
    for absent in (first, second):
        assert f"path missing: {absent}" in unopened.stderr, unopened.stderr
    assert not log.exists(), "open launched the head of a list it was going to refuse"

    # A mixed list names each operand for its own reason. `rm` used to name the
    # missing path and say nothing about the directory beside it.
    folder = room / "dirA"
    folder.mkdir()
    mixed = run(data, "--yes", "rm", "--", str(folder), str(first))
    assert mixed.returncode == 2, mixed.stderr
    assert f"path missing: {first}" in mixed.stderr, mixed.stderr
    assert f"{folder} is a directory" in mixed.stderr, mixed.stderr
    assert folder.is_dir()

    # A missing opener is the exception and is said once: it is the same answer
    # for every path in the list, which is why it was settled once to begin
    # with. Same rule as `trash`'s missing gio.
    nowhere = room / "nobin"
    nowhere.mkdir()
    unopenable = run(data, "open", str(keeper), str(keeper),
                     extra_env={"PATH": str(nowhere)})
    assert unopenable.returncode == 1, unopenable.stderr
    assert unopenable.stderr.count("no xdg-open") == 1, unopenable.stderr

def check_reindex_names_the_path_it_walked_away_from(data, workspace):
    """--quiet is "No stderr except errors", and every exit 1 here is an error.

    Four of reindex's five ways of walking away from a path went through warn(),
    which returns early under --quiet -- so the run exited 1 with nothing on
    either stream and no path named, which is the worst answer the verb that
    repairs the index can give. Only the refused xattr write survived the flag,
    because that one already called fail()."""
    room = workspace / "walkedaway"
    room.mkdir()
    probe = room / "probe.txt"
    probe.write_text("p")
    try:
        os.setxattr(probe, "user.omafiles.tags", b"probe")
    except OSError:
        return                                   # no user.* here, nothing to say

    def invalid_no_row(subject):
        os.setxattr(subject, "user.omafiles.tags", b"BAD TAG!")

    def invalid_with_row(subject):
        run(data, "tag", "add", "--tag", "kept", "--", str(subject))
        os.setxattr(subject, "user.omafiles.tags", b"Bad Tag!")

    def empty_with_row(subject):
        run(data, "tag", "add", "--tag", "kept", "--", str(subject))
        os.removexattr(subject, "user.omafiles.tags")

    def unreadable(subject):
        os.setxattr(subject, "user.omafiles.tags", b"kept")
        subject.chmod(0o000)

    causes = (("invalid-no-row", invalid_no_row, []),
              ("invalid-with-row", invalid_with_row, ["--from-xattr"]),
              ("empty-with-row", empty_with_row, ["--from-xattr"]),
              ("unreadable", unreadable, []))
    for name, prepare, flags in causes:
        where = room / name
        where.mkdir()
        subject = where / "s.txt"
        subject.write_text("s")
        prepare(subject)
        try:
            # The same answer with the flag and without it: --quiet takes the
            # note away and never the status, and this is not a note.
            for globals_ in ([], ["--quiet"]):
                walked = run(data, *globals_, "reindex", *flags, str(where))
                assert walked.returncode == 1, (name, globals_, walked.stdout)
                assert str(subject.resolve()) in walked.stderr, \
                    (name, globals_, walked.stderr)
        finally:
            subject.chmod(0o644)


def check_tag_clear_does_not_claim_a_drop_it_did_not_make(data, workspace):
    """verify names `tag clear` as the repair for xattr-invalid, and it exited 0
    having repaired nothing.

    The value stayed on the file, verify printed the same line run after run and
    nothing but a chmod settled it. A path with no index row has no record of
    the drop, so exit 0 there asserts a removal that did not happen. A path that
    has a row is the other way about, for the reason `tag add` gives: the row is
    the record, and a filesystem that will not take the second write must not
    make every clear on it fail."""
    room = workspace / "clearing"
    room.mkdir()
    stubborn = room / "invalid.txt"
    stubborn.write_text("s")
    try:
        os.setxattr(stubborn, "user.omafiles.tags", b"BAD TAG!")
    except OSError:
        return                                   # no user.* here, nothing to say
    stubborn.chmod(0o444)
    try:
        named = run(data, "verify", str(room))
        assert named.returncode == 1
        assert "xattr-invalid" in named.stdout and "tag clear" in named.stdout, named.stdout

        refused = run(data, "tag", "clear", "--", str(stubborn))
        assert refused.returncode == 1, refused.stderr
        assert "Permission denied" in refused.stderr, refused.stderr
        assert os.getxattr(stubborn, "user.omafiles.tags") == b"BAD TAG!"
        # Still the same problem, and the repair it names now says which case it
        # cannot settle -- the way xattr-differs already did.
        assert run(data, "verify", str(room)).returncode == 1
        reference = " ".join((ROOT / "docs/cli.md").read_text().split())
        assert "can only do it where the file's xattr is writable" in reference

        # A row is a record of the drop, so that one exits 0 over the identical
        # refused write.
        rowed = room / "rowed.txt"
        rowed.write_text("r")
        assert run(data, "tag", "add", "--tag", "keep", "--", str(rowed)).returncode == 0
        rowed.chmod(0o444)
        try:
            cleared = run(data, "tag", "clear", "--", str(rowed))
            assert cleared.returncode == 0, cleared.stderr
            assert "Permission denied" in cleared.stderr, cleared.stderr
            assert run(data, "tag", "ls", "--", str(rowed)).stdout.strip() == ""
        finally:
            rowed.chmod(0o644)
    finally:
        stubborn.chmod(0o644)

    # And once the permission is what it has to be, the value goes and the tree
    # is clean -- the only thing the nonzero exit was asking for.
    assert run(data, "tag", "clear", "--", str(stubborn)).returncode == 0
    assert "user.omafiles.tags" not in os.listxattr(stubborn)



def check_tag_rm_does_not_claim_a_drop_it_did_not_make(data, workspace):
    """A refused xattr drop on a path with no row is not a successful `tag rm`.

    The ghost tag stays on the file and the run exits 1 naming the refusal,
    including under `--quiet`, because nothing recorded the drop. Where a row
    existed, removing the last tag is the record -- the same reason `tag clear`
    exits 0 over a refused mirror."""
    room = workspace / "rmghost"
    room.mkdir()
    stubborn = room / "ghost.txt"
    stubborn.write_text("g")
    try:
        os.setxattr(stubborn, "user.omafiles.tags", b"ghost")
    except OSError:
        return                                   # no user.* here, nothing to say
    stubborn.chmod(0o444)
    try:
        for globals_ in ([], ["--quiet"]):
            refused = run(data, *globals_, "tag", "rm", "--tag", "ghost", "--", str(stubborn))
            assert refused.returncode == 1, (globals_, refused.stderr)
            assert "Permission denied" in refused.stderr, (globals_, refused.stderr)
        assert os.getxattr(stubborn, "user.omafiles.tags") == b"ghost"

        rowed = room / "rowed.txt"
        rowed.write_text("r")
        assert run(data, "tag", "add", "--tag", "ghost", "--", str(rowed)).returncode == 0
        rowed.chmod(0o444)
        try:
            removed = run(data, "tag", "rm", "--tag", "ghost", "--", str(rowed))
            assert removed.returncode == 0, removed.stderr
            assert "Permission denied" in removed.stderr, removed.stderr
            assert run(data, "tag", "ls", "--", str(rowed)).stdout.strip() == ""
        finally:
            rowed.chmod(0o644)
    finally:
        stubborn.chmod(0o644)

    assert run(data, "tag", "rm", "--tag", "ghost", "--", str(stubborn)).returncode == 0
    assert "user.omafiles.tags" not in os.listxattr(stubborn)

def check_completion_declines_a_flag_the_verb_refuses(data, workspace):
    """__complete offered the flags `ls` was about to refuse.

    --tag, --any and --since are mutually exclusive with --untagged, and each
    was offered once the other was on the line -- so the tab suggested exactly
    the command the parse then turned down. clap_complete derives the offer from
    `conflicts_with` and git's completion drops a conflicting option the same
    way. Measured against the refusal itself rather than a list written out
    here, so the two cannot drift."""
    room = workspace / "conflicting"
    room.mkdir()
    (room / "bare.txt").write_text("b")

    beside_untagged = run(data, "__complete", "--", "ls", "--untagged", "-").stdout.split()
    # A completion that offered nothing would pass every check below by saying
    # nothing at all.
    assert "--ext" in beside_untagged and "--json" in beside_untagged, beside_untagged

    for flag, value in (("--tag", "anything"), ("--any", "anything"), ("--since", "1h")):
        clash = run(data, "ls", "--untagged", flag, value, str(room))
        assert clash.returncode == 1, (flag, clash.stdout)
        assert "opposite questions" in clash.stderr, (flag, clash.stderr)
        assert flag not in beside_untagged, (flag, beside_untagged)

        # And the other way about: the exclusion runs both directions, so a line
        # already carrying one of them is not offered --untagged.
        spellings = [[flag]] + ([["-t"]] if flag == "--tag" else [])
        for spelling in spellings:
            offered = run(data, "__complete", "--", "ls", *spelling, value, "-").stdout.split()
            assert "--untagged" not in offered, (spelling, offered)
            assert "--ext" in offered, (spelling, offered)


def check_a_no_op_clear_invents_no_date(data, workspace):
    """`tagged` is when the tags last changed, and absent when that is unknown.

    `tag clear` on a path with no row stamped its receipt with the wall clock at
    run time -- a date for a change that never happened, on a store the run left
    untouched. `tag rm` had the same no-op right all along: a write that changes
    no tag carries the stamp the row had, which for a path with no row is
    none."""
    room = workspace / "no-op-clear"
    room.mkdir()
    never = room / "never.txt"
    never.write_text("n")

    cleared = json.loads(run(data, "--json", "tag", "clear", "--", str(never)).stdout)
    assert cleared == [], cleared
    # The sibling no-op, which is the shape it has to match.
    removed = json.loads(run(data, "--json", "tag", "rm", "--tag", "q", "--",
                             str(never)).stdout)
    assert removed == [], removed

    # A clear that does drop tags is a tagging event, so it is dated.
    dated = room / "dated.txt"
    dated.write_text("d")
    run(data, "tag", "add", "--tag", "going", "--", str(dated))
    dropped = json.loads(run(data, "--json", "tag", "clear", "--", str(dated)).stdout)
    assert dropped[0]["tags"] == [], dropped
    assert dropped[0]["tagged"] > 0, dropped


def check_a_path_it_may_not_look_at_is_not_a_path_that_is_gone(data, workspace):
    """Five verbs answered `path missing` about a file that was sitting there.

    `canonicalPath` asked `QFileInfo::exists()`, which is false for a path the
    system will not describe as well as for one that is not there, and every
    operand of `rm`, `trash`, `mv`, `cp` and `open` resolves through it. So at
    the same instant `ls` reported the row as one it could not look at and
    exited 1, `omafiles rm shut/s.txt` said `path missing` and exited 2, while
    /usr/bin/rm said `Permission denied` and exited 1 over that path. 2 is a
    path that is not there; a directory somebody chmod'd shut is the 1 the table
    gives an answer the run could not finish."""
    room = workspace / "shut-operands"
    shut = room / "shut"
    shut.mkdir(parents=True)
    subject = shut / "s.txt"
    subject.write_text("s")
    spare = room / "ok.txt"
    spare.write_text("o")
    assert run(data, "tag", "add", "--tag", "shutop", "--", str(subject)).returncode == 0
    resolved = str(subject.resolve())

    # A private PATH holding fake openers, so nothing on this machine's desktop
    # is ever started. Both of them: `open` prefers omarchy-launch-editor for
    # text/plain, so a fake xdg-open alone would launch the real editor.
    fakebin = room / "bin"
    fakebin.mkdir()
    log = room / "opened.log"
    for name in ("xdg-open", "omarchy-launch-editor"):
        fake = fakebin / name
        fake.write_text(f'#!/bin/sh\necho "$1" >> {log}\n')
        fake.chmod(0o755)
    only_fake = {"PATH": str(fakebin)}

    landing = room / "landed.txt"
    verbs = (("--yes", "rm"), ("--yes", "trash"), ("mv",), ("cp",), ("open",))

    def operands(verb):
        return [str(subject)] + ([str(landing)] if verb in ("mv", "cp") else [])

    shut.chmod(0o000)
    try:
        for globals_ in verbs:
            verb = globals_[-1]
            args = (*globals_, *operands(verb))
            env = only_fake if verb == "open" else None
            refused = run(data, *args, extra_env=env)
            assert refused.returncode == 1, (verb, refused.returncode, refused.stderr)
            assert resolved in refused.stderr, (verb, refused.stderr)
            assert "Permission denied" in refused.stderr, (verb, refused.stderr)
            assert "path missing" not in refused.stderr, (verb, refused.stderr)
            assert not landing.exists(), verb
        assert not log.exists(), "open launched a path it could not resolve"

        # A path that really is gone is still the other answer, from the same
        # five: the exit code has to stay worth reading.
        for globals_ in verbs:
            verb = globals_[-1]
            gone = [str(room / "not-here.txt")] + ([str(landing)] if verb in ("mv", "cp") else [])
            absent = run(data, *globals_, *gone,
                         extra_env=only_fake if verb == "open" else None)
            assert absent.returncode == 2, (verb, absent.returncode, absent.stderr)
            assert "path missing" in absent.stderr, (verb, absent.stderr)

        # One of each names both and reports the missing path, which is the
        # settled answer of the two.
        mixed = run(data, "--yes", "rm", str(room / "not-here.txt"), str(subject))
        assert mixed.returncode == 2, mixed.stderr
        assert "path missing" in mixed.stderr and "Permission denied" in mixed.stderr, \
            mixed.stderr

        # The destination side already answered this way, and still does: the
        # source is there, the directory it is asked to land in is not readable.
        blocked = run(data, "mv", str(spare), str(shut / "new.txt"))
        assert blocked.returncode == 1, blocked.stderr
        assert "Permission denied" in blocked.stderr, blocked.stderr
        assert spare.exists()

        # The read verbs are untouched: `ls` skips the row and says so, `prune`
        # steps over it with the run still complete, `verify` calls it
        # `unreadable`.
        listed = run(data, "ls", "--tag", "shutop")
        assert listed.returncode == 1 and resolved not in listed.stdout, listed.stdout
        assert resolved in listed.stderr, listed.stderr
        kept = run(data, "--yes", "prune", str(room))
        assert kept.returncode == 0, kept.stderr
        assert resolved not in kept.stdout, "prune dropped a row it could not look at"
        checked = run(data, "verify", str(room))
        assert checked.returncode == 1
        assert f"unreadable\t{resolved}" in checked.stdout, checked.stdout
    finally:
        shut.chmod(0o755)

    # The row is still there to act on, and the chmod undone every verb answers
    # again -- the 1 was about the permission and not about the file.
    assert resolved in run(data, "ls", "--tag", "shutop").stdout
    assert run(data, "mv", str(subject), str(landing)).returncode == 0
    assert run(data, "--yes", "rm", str(landing)).returncode == 0


def check_a_batch_carries_a_line_that_did_not_settle(data, workspace):
    """batch dropped the status of a line that ran to the end and failed.

    A line that aborts is propagated -- a missing path gives exit 2 and a full
    rollback -- and one that completes with a nonzero status was reported as
    success: `reindex` over a file whose xattr the filesystem refuses exits 1 on
    its own and exited 0 inside a batch, and so did `tag clear` on a path with
    no row whose removal was refused. `omafiles batch cleanup.txt && echo done`
    printed done with the value still on the file. The rows did land, so a
    rollback would be the wrong answer: the commit stands and the batch carries
    the 1 the line would have exited with."""
    room = workspace / "batch-unsettled"
    room.mkdir()
    subject = room / "a.txt"
    subject.write_text("a")
    assert run(data, "tag", "add", "--tag", "rowtag", "--", str(subject)).returncode == 0
    try:
        os.setxattr(subject, "user.omafiles.tags", b"filetag")
    except OSError:
        return                                   # no user.* here, nothing to say

    witness = room / "witness.txt"
    witness.write_text("w")

    subject.chmod(0o444)
    try:
        # The line's own status, measured first: index and file disagree and the
        # file will not take the write that would settle them.
        alone = run(data, "reindex", str(room))
        assert alone.returncode == 1, alone.stderr
        assert "Permission denied" in alone.stderr, alone.stderr

        batched = run(data, "batch", "-", stdin=(
            f"tag add --tag committed -- {witness}\n"
            f"reindex {room}\n"
        ))
        assert batched.returncode == 1, batched.stderr
        assert "Permission denied" in batched.stderr, batched.stderr
        # Committed, not rolled back: the rows that landed are the whole point
        # of not calling this an abort.
        assert "nothing was written" not in batched.stderr, batched.stderr
        assert run(data, "tag", "ls", "--", str(witness)).stdout.split() == ["committed"]

        # The refusal `tag add` promises nothing about is still exit 0, inside a
        # batch as on its own: the row is the record.
        added = run(data, "batch", "-",
                    stdin=f"tag add --tag mirrored -- {subject}\n")
        assert added.returncode == 0, added.stderr
        assert "Permission denied" in added.stderr, added.stderr
    finally:
        subject.chmod(0o644)

    # A clear on a path with no row is the other line that completes and fails:
    # the value is still on the file and nothing recorded the drop.
    stubborn = room / "invalid.txt"
    stubborn.write_text("s")
    os.setxattr(stubborn, "user.omafiles.tags", b"BAD TAG!")
    stubborn.chmod(0o444)
    cleanup = room / "cleanup.txt"
    cleanup.write_text(f"tag clear -- {stubborn}\n")
    try:
        assert run(data, "tag", "clear", "--", str(stubborn)).returncode == 1
        cleared = run(data, "batch", str(cleanup))
        assert cleared.returncode == 1, cleared.stderr
        assert os.getxattr(stubborn, "user.omafiles.tags") == b"BAD TAG!"
        # And a row is a record of the drop, so that one is exit 0 either way.
        rowed = room / "rowed.txt"
        rowed.write_text("r")
        assert run(data, "tag", "add", "--tag", "keep", "--", str(rowed)).returncode == 0
        rowed.chmod(0o444)
        try:
            kept = run(data, "batch", "-", stdin=f"tag clear -- {rowed}\n")
            assert kept.returncode == 0, kept.stderr
        finally:
            rowed.chmod(0o644)
    finally:
        stubborn.chmod(0o644)

    # Once the permissions allow it, the same batch settles and exits 0.
    assert run(data, "batch", str(cleanup)).returncode == 0
    assert "user.omafiles.tags" not in os.listxattr(stubborn)


def check_import_reads_either_shape_export_writes(data, workspace):
    """`export | import -` broke in a shell that exports OMAFILES_JSON.

    `--json` is a global with an environment default behind it, so both ends of
    the pipe the declaration advertises took it: `export` wrote its rows as one
    array, `import` accepted jsonl only, and the pair answered `line 1 is not a
    JSON object` over a dump this program had just written -- as did `import
    dump.json` for a dump taken with `--json`. jsonl stays the interchange form
    and what `export` writes when nothing asks otherwise; a shape `export`
    writes is a dump, and every dump comes home."""
    room = workspace / "interchange"
    room.mkdir()
    subject = room / "shared.txt"
    subject.write_text("s")
    resolved = str(subject.resolve())
    # An index of its own on each end, so what comes out of the pipe is exactly
    # what went into it.
    origin = room / "xdg-origin"
    origin.mkdir()
    assert run(origin, "tag", "add", "--tag", "shipped", "--", str(subject)).returncode == 0

    both_ends = {"OMAFILES_JSON": "1"}
    shipped = run(origin, "export", extra_env=both_ends)
    assert shipped.returncode == 0, shipped.stderr
    rows = json.loads(shipped.stdout)
    assert [row["path"] for row in rows] == [resolved], rows

    # The far end of the pipe, with the same environment on it.
    elsewhere = room / "xdg"
    elsewhere.mkdir()
    merged = run(elsewhere, "import", "-", stdin=shipped.stdout, extra_env=both_ends)
    assert merged.returncode == 0, merged.stderr
    landed = [json.loads(line) for line in run(elsewhere, "export").stdout.splitlines()]
    assert landed == rows, landed

    # A dump taken with --json is a file that can be read back.
    dump = room / "dump.json"
    dump.write_text(shipped.stdout)
    again = room / "xdg2"
    again.mkdir()
    assert run(again, "import", str(dump)).returncode == 0
    assert [json.loads(line) for line in run(again, "export").stdout.splitlines()] == rows

    # And jsonl, the default and the interchange form, is untouched.
    plain = run(origin, "export")
    third = room / "xdg3"
    third.mkdir()
    assert run(third, "import", "-", stdin=plain.stdout).returncode == 0
    assert [json.loads(line) for line in run(third, "export").stdout.splitlines()] == rows

    # Every document is still validated, and named where a diagnostic can point
    # at it: an array has rows, a jsonl dump has lines.
    bad_row = room / "bad.json"
    bad_row.write_text(json.dumps([{"path": resolved, "tags": ["fine"]},
                                   {"path": resolved, "tags": ["Bad Tag"]}]))
    refused = run(data, "import", str(bad_row))
    assert refused.returncode == 1
    assert "row 2" in refused.stderr and "invalid tag" in refused.stderr, refused.stderr

    not_an_object = room / "junk.json"
    not_an_object.write_text('[{"path": "/tmp/x", "tags": []}, 3]')
    assert "row 2" in run(data, "import", str(not_an_object)).stderr

    truncated = room / "half.json"
    truncated.write_text('[{"path": "/tmp/x", "tags": []}')
    half = run(data, "import", str(truncated))
    assert half.returncode == 1 and "JSON array" in half.stderr, half.stderr

    bad_line = room / "bad.jsonl"
    bad_line.write_text(json.dumps({"path": resolved, "tags": []}) + "\nnot json\n")
    lines = run(data, "import", str(bad_line))
    assert lines.returncode == 1 and "line 2" in lines.stderr, lines.stderr

    # Nothing any of those refused reached the index.
    assert run(data, "tag", "ls", "--", str(subject)).stdout.strip() == ""


def check_a_batch_line_is_not_a_shell_line(data, workspace):
    """The grammar a batch line follows, and the one rule it does not share with a shell.

    A line is split into words the way a shell splits them and nothing else is
    expanded, which was written down nowhere -- and `#` is a comment only in the
    first column. `tag add --tag t -- c.txt # a note` hands three more operands
    to the verb, and the only thing that ever said so was the `path missing` the
    line then failed with."""
    room = workspace / "batchgrammar"
    room.mkdir()
    spaced = room / "b with space.txt"
    hashed = room / "#draft.md"
    plain = room / "plain.txt"
    for path in (spaced, hashed, plain):
        path.write_text("x")

    def lines(*given, globals_=(), expect=0):
        got = run(data, *globals_, "batch", "-", stdin="".join(f"{line}\n" for line in given))
        assert got.returncode == expect, (got.returncode, got.stderr)
        return got

    # A comment line, a blank one and one holding nothing but spaces are all
    # skipped, and the line under them still runs.
    lines("# a whole-line comment", "", "   ",
          f"tag add --tag grammar -- {plain}")
    assert run(data, "tag", "ls", "--", str(plain)).stdout.split() == ["grammar"]

    # Quotes of either kind hold a word together, and a backslash escapes what
    # follows. Splitting on whitespace alone would make each of these two paths.
    escaped = str(spaced).replace(" ", "\\ ")
    lines(f'tag add --tag quoted -- "{spaced}"')
    lines(f"tag add --tag single -- '{spaced}'")
    lines(f"tag add --tag backslash -- {escaped}")
    assert run(data, "tag", "ls", "--", str(spaced)).stdout.split() \
        == ["backslash", "quoted", "single"]

    # A quote nothing closes is refused with the line quoted, and nothing lands.
    unterminated = lines(f'tag add --tag never -- "{plain}', expect=1)
    assert "line 1: unterminated double quote" in unterminated.stderr, unterminated.stderr
    assert "never" not in run(data, "tag", "ls", "--", str(plain)).stdout

    # Nothing is expanded: no $VAR and no ~, so both arrive as the words they are.
    literal = lines("tag add --tag nope -- $OMAFILES_NO_SUCH_VAR/x.txt ~/x.txt", expect=2)
    assert "$OMAFILES_NO_SUCH_VAR/x.txt" in literal.stderr, literal.stderr
    assert "~/x.txt" in literal.stderr, literal.stderr

    # And the rule that is not the shell's: `#` after arguments is a word. The
    # line fails with `path missing` about each of them and the batch rolls back.
    trailing = lines(f"tag add --tag trap -- {plain}   # a note", expect=2)
    for word in ("path missing: #", "path missing: a", "path missing: note"):
        assert word in trailing.stderr, trailing.stderr
    assert "trap" not in run(data, "tag", "ls", "--", str(plain)).stdout

    # And the first column means the first column: an indented `#` is a word
    # like any other, so the line is a verb nobody declared and the run says so
    # rather than skipping the line.
    indented = lines("   # not a comment", expect=1)
    assert "unknown command: #" in indented.stderr, indented.stderr

    # Which is what buys this: a file really called `#draft.md` needs no quoting.
    lines(f"tag add --tag hashname -- {hashed}")
    assert run(data, "tag", "ls", "--", str(hashed)).stdout.split() == ["hashname"]

    # reindex, prune and import are lines like any other write verb.
    dump = room / "dump.jsonl"
    dump.write_text(run(data, "export").stdout)
    lines(f"reindex {room}", f"import {dump}")

    # `prune` asks, and a batch off a terminal has nowhere to ask: the line
    # refuses and takes the run with it unless the batch itself carries --yes.
    plain.unlink()
    refused = lines(f"prune {room}", expect=1)
    assert "without a terminal to ask at" in refused.stderr, refused.stderr
    assert str(plain.resolve()) in run(data, "export").stdout, "the rollback lost a row"
    lines(f"prune {room}", globals_=("--yes",))
    assert str(plain.resolve()) not in run(data, "export").stdout

    # Written down where a reader looks for it, not only in the failure.
    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    assert "in the first column is a comment" in reference
    assert "no `$VAR`, no `~`, no globbing" in reference


def check_verify_says_only_what_it_did(data, workspace):
    """Two lines about one chmod, the second of which contradicted the first.

    `unreadable dir  nothing under it was checked` sat under a row from beneath
    that same directory, which had been checked and reported -- so the detail was
    false on the line above it. It says what the walk did instead. The text form
    is the other half: it cannot spell a path holding a tab or a newline, and
    only --json can."""
    room = workspace / "onlywhatitdid"
    room.mkdir()
    shut = room / "shut"
    shut.mkdir()
    inside = shut / "f.txt"
    inside.write_text("f")
    assert run(data, "tag", "add", "--tag", "sealed", "--", str(inside)).returncode == 0
    resolved = str(inside.resolve())

    shut.chmod(0o000)
    try:
        seen = run(data, "verify", str(room))
        assert seen.returncode == 1
        # The row underneath is still reported, so the directory's detail has to
        # be true beside it.
        assert f"unreadable\t{resolved}" in seen.stdout, seen.stdout
        assert f"unreadable\t{shut.resolve()}\tthe tree under it was not walked" \
            in seen.stdout, seen.stdout
        assert "nothing under it was checked" not in seen.stdout, seen.stdout
    finally:
        shut.chmod(0o755)

    # A tab in a name is the separator, and a newline is the end of the line:
    # the text form cannot tell either apart, and says so in the documentation
    # rather than leaving a reader to find out from a mangled line.
    tabbed = room / "tab\there.txt"
    broken = room / "new\nline.txt"
    tabbed.write_text("t")
    broken.write_text("n")
    run(data, "tag", "add", "--tag", "sealed", "--", str(tabbed), str(broken))
    tabbed.unlink()
    broken.unlink()
    text = run(data, "verify", str(room))
    assert text.returncode == 1
    ambiguous = [line for line in text.stdout.splitlines() if line.startswith("missing\t")]
    assert any(line.count("\t") == 2 for line in ambiguous), ambiguous
    assert any(line.endswith("here.txt") for line in ambiguous), ambiguous

    rows = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
    named = {row["path"] for row in rows}
    assert str(tabbed.resolve()) in named and str(broken.resolve()) in named, rows

    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    assert "cannot spell a path holding a tab or a newline" in reference


def check_a_preview_runs_where_a_read_runs(data, workspace):
    """A dry run took the write lock it never uses.

    With the index directory read-only, `--dry-run mv` and `--dry-run rm` exited
    3 with `store: cannot lock` while `ls`, `export` and `verify` answered from
    the same index -- a preview that could not run where a read could. It
    commits nothing, so it loads the index the way a read does. And it prints
    what the real run prints: `mv` and `cp` said `old -> new` in a dry run and
    the destination alone in a real one."""
    room = workspace / "preview"
    room.mkdir()
    own = room / "xdg"
    own.mkdir()
    subject = room / "a.txt"
    subject.write_text("a")
    assert run(own, "tag", "add", "--tag", "previewed", "--", str(subject)).returncode == 0
    landing = room / "b.txt"

    (own / "omafiles").chmod(0o555)
    try:
        for reader in (("ls", "--tag", "previewed"), ("export",), ("verify", str(room))):
            got = run(own, *reader)
            assert got.returncode == 0, (reader, got.stderr)
        for preview in (("mv", str(subject), str(landing)), ("rm", str(subject))):
            got = run(own, "--dry-run", *preview)
            assert got.returncode == 0, (preview, got.stderr)
            assert "cannot lock" not in got.stderr, (preview, got.stderr)
        # The real write still needs the lock and still says so.
        blocked = run(own, "mv", str(subject), str(landing))
        assert blocked.returncode == 3 and "cannot lock" in blocked.stderr, blocked.stderr
    finally:
        (own / "omafiles").chmod(0o755)

    # The preview and the result are one shape, in text as they already are
    # under --json: both name where the file went.
    for verb in ("mv", "cp"):
        target = room / f"{verb}-landed.txt"
        planned = run(own, "--dry-run", verb, str(subject), str(target))
        assert planned.returncode == 0, planned.stderr
        assert planned.stdout.splitlines() == [str(target.resolve())], planned.stdout
        done = run(own, verb, str(subject), str(target))
        assert done.stdout == planned.stdout, (planned.stdout, done.stdout)
        if verb == "mv":
            subject = target


def check_a_message_names_what_the_verb_takes(data, workspace):
    """Two places the program described itself wrongly.

    `mv a b c` answered `mv: <old> <new> required` about a run that had supplied
    both and one more; the verb takes exactly two. And `completions --help`
    printed the generic `Globals: --json ...` footer two lines under its own
    sentence about being the one verb that refuses --json."""
    room = workspace / "namestaken"
    room.mkdir()
    one = room / "one.txt"
    two = room / "two.txt"
    one.write_text("1")
    two.write_text("2")

    for verb in ("mv", "cp"):
        for operands in ((str(one),), (str(one), str(two), str(room / "three.txt"))):
            got = run(data, verb, *operands)
            assert got.returncode == 1, (verb, got.stderr)
            assert got.stderr.strip() == \
                f"{verb}: takes exactly two paths, <old> and <new>", got.stderr
        assert not (room / "three.txt").exists()

    def globals_line(*args):
        listed = [line for line in run(data, *args).stdout.splitlines()
                  if line.startswith("Globals:")]
        assert len(listed) == 1, listed
        return listed[0]

    assert globals_line("completions", "--help") == "Globals: --dry-run  --yes  --quiet"
    assert globals_line("ls", "--help") == "Globals: --json  --dry-run  --yes  --quiet"
    refused = run(data, "--json", "completions", "bash")
    assert refused.returncode == 1 and "has no meaning here" in refused.stderr, refused.stderr


def check_a_name_the_walk_cannot_spell_is_reported(data, workspace):
    """A filename that is not valid UTF-8 left the walk one row short in silence.

    A path here is text: the index is UTF-8 by definition and so is an xattr
    value, so such a name can be acted on by nothing in this program. That limit
    is written down; the report was not. `ls --untagged` dropped the entry
    without a word and exited 0, which reads as "there is nothing untagged here"
    about a directory holding three untagged entries -- one of them a file whose
    own name is perfectly spellable, and collateral from its parent's."""
    room = workspace / "unspellable"
    room.mkdir()
    here = os.fsencode(str(room.resolve()))
    folder = here + b"/b\xffd"
    loose = here + b"/f\xffle.txt"
    os.mkdir(folder)
    open(folder + b"/inner.txt", "wb").close()
    open(loose, "wb").close()
    plain = room / "ok.txt"
    plain.write_text("k")

    listed = run(data, "ls", "--untagged", str(room))
    # The part of the answer it can give is still given, on stdout and alone.
    assert listed.stdout.splitlines() == [str(plain.resolve())], listed.stdout
    # Both are named, mangled, and the file inside the folder is covered by the
    # folder the way a shut directory covers what is under it.
    assert listed.stderr.count("not valid UTF-8") == 2, listed.stderr
    assert "inner.txt" not in listed.stderr, listed.stderr
    # A list one row short is not the answer that was asked for -- the same 1 a
    # directory nobody may open gets. See check_an_incomplete_answer_is_not_a_success.
    assert listed.returncode == 1, listed.stderr
    # --quiet takes the note away and leaves the status.
    hushed = run(data, "--quiet", "ls", "--untagged", str(room))
    assert hushed.returncode == 1, hushed.stderr
    assert hushed.stderr == "", hushed.stderr

    # `verify` and `reindex` walk the same entries, so the hole was theirs too.
    # `verify` says it in the shape it answers in, because a problem only stderr
    # knows about is one a --json consumer cannot read.
    seen = run(data, "--json", "verify", str(room))
    assert seen.returncode == 1, seen.stderr
    problems = json.loads(seen.stdout)["problems"]
    assert sorted(problem["problem"] for problem in problems) \
        == ["unreadable", "unreadable"], problems
    assert all("not valid UTF-8" in problem["detail"] for problem in problems), problems

    rebuilt = run(data, "reindex", str(room))
    assert rebuilt.stderr.count("not valid UTF-8") == 2, rebuilt.stderr
    assert rebuilt.returncode == 1, rebuilt.stderr

    # `prune` walks the index and not the disk, so it never had the hole: a row
    # can only hold a name that was spellable when it was written.
    assert run(data, "--yes", "prune", str(room)).returncode == 0

    # Rename the files and all three walks are whole again: the status is about
    # the answer being short, not about the tree.
    os.rename(folder, here + b"/bad")
    os.rename(loose, here + b"/file.txt")
    for args in (("ls", "--untagged", str(room)), ("verify", str(room)),
                 ("reindex", str(room))):
        whole = run(data, *args)
        assert whole.returncode == 0, (args, whole.stderr)


def check_the_json_shapes_are_written_down(data, workspace):
    """Four verbs answered in JSON with no declared shape to read them by.

    `verify` says what its object holds and nothing else did, so a consumer of
    `reindex`, `prune`, `batch` or `import` was reading an undeclared shape --
    and `reindex` names a path whose mirror the filesystem refused with nothing
    in the row to mark it. The row is the index row; the refusal is the run's,
    and the run says it with exit 1 and a line on stderr."""
    room = workspace / "shapes"
    room.mkdir()
    subject = room / "s.txt"
    subject.write_text("s")
    resolved = str(subject.resolve())
    assert run(data, "tag", "add", "--tag", "shaped", "--", str(subject)).returncode == 0

    def rows(*args, **passed):
        got = run(data, "--json", *args, **passed)
        assert got.returncode == 0, (args, got.stderr)
        parsed = json.loads(got.stdout)
        assert isinstance(parsed, list), parsed
        for row in parsed:
            assert set(row) == {"path", "tags", "tagged"}, row
        return got, parsed

    # batch: one array of every row the run wrote, after the commit.
    _, written = rows("batch", "-", stdin=f"tag add --tag batched -- {subject}\n")
    assert [row["path"] for row in written] == [resolved], written

    # import: one array of the rows it merged.
    # Written by hand rather than by `export`, which dumps the whole index: the
    # shape under test is the array `import` prints, not what earlier cases left
    # in the index.
    dump = room / "dump.jsonl"
    dump.write_text(json.dumps({"path": resolved, "tags": ["merged"]}) + "\n")
    _, merged = rows("import", str(dump))
    assert [row["path"] for row in merged] == [resolved], merged

    # prune: an object holding one `dropped` array of paths.
    gone = room / "gone.txt"
    gone.write_text("g")
    run(data, "tag", "add", "--tag", "shaped", "--", str(gone))
    gone.unlink()
    dropped = run(data, "--json", "--yes", "prune", str(room))
    assert dropped.returncode == 0, dropped.stderr
    assert json.loads(dropped.stdout) == {"dropped": [str(gone.resolve())]}, dropped.stdout

    # reindex: the rows it wrote, and nothing about the file's own copy.
    try:
        os.removexattr(subject, "user.omafiles.tags")
    except OSError:
        return                               # no user.* here; nothing to measure
    os.setxattr(subject, "user.omafiles.tags", b"stale")
    subject.chmod(0o444)
    try:
        refused = run(data, "--json", "reindex", str(room))
        assert refused.returncode == 1, refused.stderr
        assert "Permission denied" in refused.stderr and resolved in refused.stderr
        named = json.loads(refused.stdout)
        assert [row["path"] for row in named] == [resolved], named
        assert set(named[0]) == {"path", "tags", "tagged"}, named

        # `import` reads that same refusal the way `tag add` does: the row is
        # the record, so the merge still exits 0.
        merged_over = run(data, "import", str(dump))
        assert merged_over.returncode == 0, merged_over.stderr
        assert "Permission denied" in merged_over.stderr, merged_over.stderr
    finally:
        subject.chmod(0o644)

    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    assert "one array holding every row it wrote" in reference, "batch has no shape"
    assert "one array of the rows it merged" in reference, "import has no shape"
    assert "an object holding one `dropped` array" in reference, "prune has no shape"
    assert "reads the array with the exit status rather than instead of it" in reference
    assert "the run still exits 0, the rule `tag add` keeps" in reference


def check_a_move_rewrites_only_the_mirror_it_left_behind(data, workspace):
    """mv writes the file's own copy of its tags only where the move dropped it.

    `rename(2)` keeps the inode, so the xattr is already there under the new
    name and writing it again could only fail: moving a folder holding a tagged
    file nobody may write reported `xattr set failed` about a mirror that was
    already correct, with the index right and the file right. Only the crossing
    copy leaves the mirror behind, and that one still has to land it."""
    room = workspace / "mirrorkept"
    (room / "top/inner").mkdir(parents=True)
    subject = room / "top/inner/v.txt"
    subject.write_text("v")
    assert run(data, "tag", "add", "--tag", "carried", "--", str(subject)).returncode == 0
    try:
        os.getxattr(subject, "user.omafiles.tags")
    except OSError:
        return                                   # no user.* here; no mirror to keep

    # Unwritable through its parent, which is what a `setxattr` on the file
    # would answer EACCES for once it has moved.
    (room / "top/inner").chmod(0o000)
    try:
        moved = run(data, "mv", str(room / "top"), str(room / "top2"))
        assert moved.returncode == 0, moved.stderr
        assert moved.stderr == "", moved.stderr
    finally:
        for shut in (room / "top2/inner", room / "top/inner"):
            if shut.is_dir():
                shut.chmod(0o755)
    landed = room / "top2/inner/v.txt"
    assert os.getxattr(landed, "user.omafiles.tags") == b"carried"
    assert run(data, "verify", str(room)).returncode == 0, "the move left the two disagreeing"

    # The other half: across a filesystem the bytes are copied and the copy
    # carries no `user.*`, so the mirror is written there or it is nowhere.
    shm = Path("/dev/shm")
    if not shm.is_dir() or os.stat(shm).st_dev == os.stat(room).st_dev:
        return                                   # one filesystem; nothing to cross
    across = Path(tempfile.mkdtemp(prefix="omafiles-mirror-", dir=str(shm)))
    try:
        try:
            os.setxattr(across, "user.omafiles.probe", b"x")
            os.removexattr(across, "user.omafiles.probe")
        except OSError:
            return                               # no user.* at the far end
        crossing = room / "crossing.txt"
        crossing.write_text("c")
        run(data, "tag", "add", "--tag", "carried", "--", str(crossing))
        arrived = across / "crossing.txt"
        crossed = run(data, "mv", str(crossing), str(arrived))
        assert crossed.returncode == 0, crossed.stderr
        assert os.getxattr(arrived, "user.omafiles.tags") == b"carried", \
            "the crossing copy landed without the file's own copy of its tags"
        assert run(data, "verify", str(across)).returncode == 0, crossed.stderr
    finally:
        shutil.rmtree(across, ignore_errors=True)


def check_a_batch_reports_the_rows_it_kept(data, workspace):
    """batch --json named rows the commit never kept.

    The array was the accumulator every line appended to, so `tag add` then
    `tag clear` on one path printed two objects over an index the run left
    empty: an intermediate state that never reached disk, and a row the clear
    had removed. Three `tag add` lines on one path printed three. What the run
    wrote is what the index holds when it ends, and `export` is where that is
    legible -- the dry run had the same double vision, one line of text per line
    of the file rather than per path."""
    room = workspace / "batchrows"
    room.mkdir()
    subject = room / "kept.txt"
    other = room / "beside.txt"
    subject.write_text("k")
    other.write_text("b")
    resolved = str(subject.resolve())

    def held(*paths):
        """The index rows for these paths, as `export` shows them."""
        wanted = set(paths)
        rows = [json.loads(line) for line in run(data, "export").stdout.splitlines()]
        return sorted((row for row in rows if row["path"] in wanted),
                      key=lambda row: row["path"])

    # Written and taken away again inside one run: the commit kept no row, so
    # there is none to report.
    cleared = run(data, "--json", "batch", "-", stdin=(
        f"tag add --tag one -- {subject}\n"
        f"tag clear -- {subject}\n"
    ))
    assert cleared.returncode == 0, cleared.stderr
    assert json.loads(cleared.stdout) == [], cleared.stdout
    assert held(resolved) == [], "the batch left a row it reported clearing"

    # Once per path and not once per line, and the row is the one that landed:
    # the array is what `export` shows for the paths the run touched.
    many = run(data, "--json", "batch", "-", stdin=(
        f"tag add --tag one -- {subject}\n"
        f"tag add --tag two -- {subject}\n"
        f"tag add --tag three -- {subject}\n"
        f"tag add --tag one -- {other}\n"
    ))
    assert many.returncode == 0, many.stderr
    reported = sorted(json.loads(many.stdout), key=lambda row: row["path"])
    assert reported == held(resolved, str(other.resolve())), (reported, many.stdout)
    assert [row["tags"] for row in reported] == [["one"], ["one", "three", "two"]], reported

    # The dry run reads the same accumulator, so it names each path once too.
    dry = run(data, "--dry-run", "batch", "-", stdin=(
        f"tag add --tag four -- {subject}\n"
        f"tag add --tag five -- {subject}\n"
    ))
    assert dry.returncode == 0, dry.stderr
    assert dry.stdout.splitlines() == [resolved], dry.stdout


def check_an_import_reports_one_row_per_path(data, workspace):
    """import --json printed a row per dump line rather than per path.

    Two lines naming one path printed the state after the first as well as the
    merge -- one row in the index and two in the array, the first of them a
    state that never reached disk."""
    room = workspace / "importrows"
    room.mkdir()
    subject = room / "m.txt"
    subject.write_text("m")
    resolved = str(subject.resolve())

    dump = room / "twice.jsonl"
    dump.write_text(
        json.dumps({"path": resolved, "tags": ["x"], "tagged": 1}) + "\n"
        + json.dumps({"path": resolved, "tags": ["y"], "tagged": 2}) + "\n")

    merged = run(data, "--json", "import", str(dump))
    assert merged.returncode == 0, merged.stderr
    reported = json.loads(merged.stdout)
    rows = [json.loads(line) for line in run(data, "export").stdout.splitlines()]
    assert reported == [row for row in rows if row["path"] == resolved], reported
    assert [row["tags"] for row in reported] == [["x", "y"]], reported


def check_a_verify_detail_stays_one_field(data, workspace):
    """A tab in an xattr value made four fields of a three-field line.

    The detail quotes the bytes a file carries and those are anybody's: a tab in
    one read as the separator between fields, and a newline turned one problem
    into two lines -- on `xattr-invalid`, the kind that exists to report a value
    nothing can be read out of. The path field still cannot be escaped, because
    a consumer cutting it out of the line has to get a path back; the detail is
    prose about it."""
    room = workspace / "verifydetail"
    room.mkdir()
    subject = room / "d.txt"
    subject.write_text("d")
    resolved = str(subject.resolve())
    try:
        os.setxattr(subject, "user.omafiles.tags", b"aa\tbb")
    except OSError:
        return                                   # no user.* here, nothing to say

    tabbed = run(data, "verify", str(room))
    assert tabbed.returncode == 1, tabbed.stdout
    lines = tabbed.stdout.splitlines()
    assert len(lines) == 1, lines
    kind, path, detail = lines[0].split("\t")    # a fourth field raises here
    assert (kind, path) == ("xattr-invalid", resolved), lines
    assert "xattr=aa\\tbb;" in detail, detail

    os.setxattr(subject, "user.omafiles.tags", b"cc\ndd")
    lined = run(data, "verify", str(room))
    assert lined.stdout.count("\n") == 1, lined.stdout
    assert "xattr=cc\\ndd;" in lined.stdout, lined.stdout

    # A backslash the value carries is doubled, so a real tab and the two
    # characters that spell one do not print the same.
    os.setxattr(subject, "user.omafiles.tags", b"ee\\tff")
    literal = run(data, "verify", str(room))
    assert "xattr=ee\\\\tff;" in literal.stdout, literal.stdout

    # --json carries the bytes themselves, which is the other half of why it is
    # the shape to parse.
    os.setxattr(subject, "user.omafiles.tags", b"gg\thh")
    problems = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
    assert [p["detail"].split("xattr=")[1].split(";")[0] for p in problems] \
        == ["gg\thh"], problems

    # A value that is not valid UTF-8 is mangled the way a name that is not is,
    # in both forms -- and both places a reader looks say so.
    os.setxattr(subject, "user.omafiles.tags", b"ii\xffjj")
    mangled = run(data, "verify", str(room))
    assert "\ufffd" in mangled.stdout, mangled.stdout
    shaped = json.loads(run(data, "--json", "verify", str(room)).stdout)
    assert "\ufffd" in shaped["problems"][0]["detail"], shaped
    os.removexattr(subject, "user.omafiles.tags")

    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    assert "a tab there is written `\\t`, a newline `\\n`" in reference
    assert "U+FFFD" in reference


def check_batch_json_prints_nothing_on_rollback(data, workspace):
    """`--json` is the receipt after a commit. A rolled-back batch is not one.

    A line that fails returns before anything reached the disk. Successful
    array verbs print `[]` for an empty answer; a refusal is stderr and a
    status, with nothing on stdout -- the same as a `tag add` that never wrote.
    """
    room = workspace / "batch-json-rollback"
    room.mkdir()
    doomed = room / "doomed.txt"
    doomed.write_text("x")
    missing = room / "not-here.txt"
    rolled = run(data, "--json", "batch", "-", stdin=(
        f"tag add --tag rolled -- {doomed}\n"
        f"tag add --tag rolled -- {missing}\n"
    ))
    assert rolled.returncode == 2, rolled.stderr
    assert "line 2" in rolled.stderr and "not-here.txt" in rolled.stderr
    assert rolled.stdout == "", rolled.stdout
    assert run(data, "tag", "ls", "--", str(doomed)).stdout.strip() == ""
    try:
        assert "user.omafiles.tags" not in os.listxattr(doomed)
    except OSError:
        pass


def check_has_on_a_path_nobody_may_look_at(data, workspace):
    """`has` is exit 1 over a path the system will not describe, not a silent no.

    `--json` is for a genuine yes or no. A refusal is stderr and a status, with
    no object. A genuine no on a file anybody may look at is exit 1 with empty
    stderr -- that is `has --tag absent`, not this case.
    """
    room = workspace / "has-shut"
    shut = room / "shut"
    shut.mkdir(parents=True)
    subject = shut / "s.txt"
    subject.write_text("s")
    assert run(data, "tag", "add", "--tag", "haslook", "--", str(subject)).returncode == 0
    resolved = str(subject.resolve())

    shut.chmod(0o000)
    try:
        refused = run(data, "has", "--tag", "haslook", "--", str(subject))
        assert refused.returncode == 1, (refused.returncode, refused.stderr)
        assert "Permission denied" in refused.stderr, refused.stderr
        assert resolved in refused.stderr, refused.stderr
        assert "path missing" not in refused.stderr, refused.stderr
        assert refused.stderr != ""

        silent = run(data, "--json", "has", "--tag", "haslook", "--", str(subject))
        assert silent.returncode == 1, (silent.returncode, silent.stderr)
        assert silent.stdout == "", silent.stdout
        assert "Permission denied" in silent.stderr, silent.stderr
    finally:
        shut.chmod(0o755)

    ok = run(data, "has", "--tag", "haslook", "--", str(subject))
    assert ok.returncode == 0, ok.stderr


if __name__ == "__main__":
    main()
