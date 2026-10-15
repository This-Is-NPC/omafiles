#!/usr/bin/env python3
"""The CLI contract, end to end, against a real index in a temporary XDG home.

Every verb that is built has a case here. A verb that parses and does nothing is
indistinguishable from one that works until somebody depends on it."""

import ctypes
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


def sandbox(data_home, beside):
    """Every directory a run finds files through, none of them the user's.

    XDG_DATA_HOME is `data_home`; the other three base directories and HOME are
    under `beside`, so no check reads the real roots, cache or state."""
    return {"XDG_DATA_HOME": str(data_home),
            **{variable: str(beside / name) for variable, name in (
                ("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                ("XDG_STATE_HOME", "state"), ("HOME", "home"))}}


def cli_env(data_home):
    return {**os.environ, **sandbox(data_home, data_home)}


def run(data_home, *args, extra_env=None, stdin=None, cwd=None, preexec_fn=None, wrap=()):
    env = cli_env(data_home)
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        [*wrap, str(CLI), *map(str, args)],
        # ROOT unless a case says otherwise. The verbs that default `[dir]` to
        # the current directory can only be checked from a directory small
        # enough to state the whole answer for.
        cwd=str(cwd or ROOT),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        # Never the terminal the suite was started from: a child that inherits
        # it sees a tty, and `confirmed()` blocks on an invisible prompt.
        **({"stdin": subprocess.DEVNULL} if stdin is None else {"input": stdin}),
        preexec_fn=preexec_fn,
    )


def forge_lock(data_home):
    """The index lock QLockFile writes, naming this live process: its pid, the
    basename of /proc/<pid>/exe, which is where Qt looks, and the hostname. Qt
    reads a lock whose pid is dead, or runs something else, as stale."""
    lock = data_home / "omafiles" / "index.jsonl.lock"
    lock.parent.mkdir(parents=True, exist_ok=True)
    lock.write_text(f"{os.getpid()}\n{Path(os.readlink('/proc/self/exe')).name}\n"
                    f"{os.uname().nodename}\n")
    return lock


def run_pty(data_home, *args, answer="y\n", extra_env=None, cwd=None, wrap=()):
    """The same run(), over a real terminal.

    `confirmed()` refuses off a tty rather than asking, so every case about what
    the question says -- and about what has already happened by the time it is
    asked -- can only be driven through a pty. The answer is written before the
    prompt appears, which a line-disciplined terminal holds for it; a run that
    never asks simply echoes it back."""
    env = cli_env(data_home)
    if extra_env:
        env.update(extra_env)
    parent, child = pty.openpty()
    process = subprocess.Popen(
        [*wrap, str(CLI), *map(str, args)],
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
        check_transfer_conflict_policies(data, workspace)
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
        check_an_unplugged_root_is_not_a_deleted_file(data, workspace)
        check_a_sweep_only_ever_adds(data, workspace)
        check_a_list_of_names_is_not_the_index(data, workspace)
        check_mv_renames_a_set_by_rule(data, workspace)
        check_ls_name_asks_the_disk_or_the_list(data, workspace)
        check_recent_reads_the_desktops_list(data, workspace)
        check_chmod_sets_the_rwx_bits(data, workspace)
        check_chmod_never_touches_the_special_bits(data, workspace)
        check_chmod_refuses_a_symlink_and_a_set_it_cannot_take(data, workspace)
        check_chmod_leaves_the_owner_question_to_the_system(data, workspace)
        check_a_walk_lists_what_the_disk_holds(data, workspace)
        check_a_short_list_of_names_says_so(data, workspace)
        check_every_printed_row_is_absolute(data, workspace)
        check_chmod_takes_each_path_once(data, workspace)
        check_a_shape_needs_a_path_it_can_read(data, workspace)
        check_a_name_pattern_is_fnmatch(data, workspace)
        check_ext_refuses_what_no_suffix_is(data, workspace)
        check_a_stamp_before_1970_is_malformed(data, workspace)
        check_ls_narrows_by_kind_and_mtime(data, workspace)
        check_bytes_that_are_not_utf8_name_no_other_file(data, workspace)
        check_could_not_look_is_not_absent(data, workspace)
        check_not_a_directory_is_the_systems_answer(data, workspace)
        check_a_dangling_chain_names_its_missing_end(data, workspace)
        check_an_away_root_wins_over_the_disk(data, workspace)
        check_a_write_error_names_its_own_reason(data, workspace)
        check_a_row_under_a_file_is_gone(data, workspace)
        check_a_directory_is_shut_when_it_will_not_open(data, workspace)
        check_the_window_refuses_what_it_cannot_spell(data, workspace)
        check_desktop_file_manager(data, workspace)
        check_recent_orders_before_it_looks(data, workspace)
        check_recent_desktop_is_one_row_per_file(data, workspace)
        check_open_names_every_path_before_the_opener(data, workspace)
        check_a_tag_verb_starts_from_the_files_own_tags(data, workspace)
        check_removing_no_value_is_not_a_refusal(data, workspace)
        check_tag_merge_says_the_mirror_it_skipped(data, workspace)
        check_a_flag_takes_one_value_once(data, workspace)
        check_completion_offers_saved_names(data, workspace)
        check_a_batch_vets_every_line_first(data, workspace)
        check_a_batch_writes_each_mirror_once(data, workspace)
        check_sweep_answers_whole(data, workspace)
        check_the_store_says_why(data, workspace)
        check_a_root_is_declared_and_undeclared_truly(data, workspace)
        check_an_input_says_why_it_will_not_read(data, workspace)
        check_every_listing_reads_a_row_under_a_file_as_gone(data, workspace)
        check_an_away_root_wins_over_every_operand_and_walk(data, workspace)
        check_the_current_directory_is_an_input(data, workspace)
        check_a_destination_resolves_as_the_kernel_does(data, workspace)
        check_a_row_moved_out_of_a_root_is_written_where_it_is(data, workspace)
        check_the_index_holds_only_the_shape_it_writes(data, workspace)
        check_a_name_may_begin_with_a_byte_order_mark(data, workspace)
        check_a_shut_directory_says_what_the_system_said(data, workspace)
        check_import_says_a_mirror_it_did_not_attempt(data, workspace)
        check_a_copy_carries_bytes_mode_and_tags(data, workspace)
        check_cp_takes_regular_files_only(data, workspace)
        check_rm_and_trash_refuse_the_root_directory(data, workspace)
        check_completion_goes_on_past_a_broken_roots_file(data, workspace)
        check_a_relative_xdg_home_is_ignored(data, workspace)
        check_a_command_offered_to_paste_is_quoted(data, workspace)
        check_a_path_the_kernel_resolves_is_text_only_when_it_spells(data, workspace)
        check_a_path_realpath_may_not_walk_is_refused(data, workspace)
        check_each_hop_is_named_where_the_kernel_looks(data, workspace)
        check_a_trailing_slash_names_a_directory(data, workspace)
        check_an_away_root_refuses_every_operand_it_is_handed(data, workspace)
        check_a_nested_root_is_listed_once_and_only_while_here(data, workspace)
        check_a_row_now_a_symlink_is_missing(data, workspace)
        check_input_that_will_not_read_is_refused(data, workspace)
        check_sweep_and_reindex_restamp_what_they_change(data, workspace)
        check_view_and_roots_answer_as_every_writer_does(data, workspace)
        check_a_list_that_will_not_read_is_an_error(data, workspace)
        check_a_cache_it_cannot_make_says_why(data, workspace)
        check_recent_reads_only_what_glib_reads(data, workspace)
        check_a_relative_home_decides_no_directory(data, workspace)
        check_a_check_must_stay_inside_its_root(data, workspace)
        check_no_system_path_becomes_text_outside_paths(data, workspace)
        check_a_value_nobody_may_read_is_not_no_value(data, workspace)
        check_a_drop_with_no_row_is_reported(data, workspace)
        check_a_batch_line_starts_from_what_an_earlier_line_settled(data, workspace)
        check_a_copy_carries_the_files_own_value(data, workspace)
        check_a_kept_file_is_read_in_the_shape_it_is_written(data, workspace)
        check_a_batch_line_splits_at_spaces_and_tabs_only(data, workspace)
        check_completion_hands_over_the_words_up_to_the_cursor(data, workspace)
        check_what_a_run_says_about_itself_is_true(data, workspace)
        check_a_tag_is_folded_in_ascii_only(data, workspace)
        check_a_flag_from_the_environment_is_on_or_off(data, workspace)
        check_tag_merge_onto_itself_is_refused(data, workspace)
        check_tag_rename_on_selected_paths(data, workspace)
        check_rename_refuses_a_late_collision(data, workspace)
        check_landing_on_a_stale_row_names_the_tags_it_drops(data, workspace)
        check_trash_lists_each_operand_before_its_rows(data, workspace)
        check_no_write_goes_through_a_link_at_a_rows_path(data, workspace)
        check_import_starts_from_the_files_own_tags(data, workspace)
        check_a_lock_nobody_wrote_is_named(data, workspace)
        check_r4_an_away_root_inside_an_operand_refuses_it(data, workspace)
        check_r4_a_link_at_a_walked_rows_path_is_untagged(data, workspace)
        check_r4_a_cached_answer_agrees_with_the_walk(data, workspace)
        check_r4_filters_follow_their_prior_art(data, workspace)
        check_r4_a_bind_mount_is_a_mount(data, workspace)
        check_r4_a_row_under_a_linked_directory_is_missing(data, workspace)
        check_r4_dot_and_root_operands_are_refused(data, workspace)
        check_r4_a_fifo_does_not_cross_in_a_dry_run(data, workspace)
        check_r4_trash_takes_an_operand_inside_another_once(data, workspace)
        check_r4_every_diagnostic_names_its_verb(data, workspace)
        check_r4_a_dangling_link_is_named_hop_by_hop(data, workspace)
        check_r4_a_list_of_names_holds_no_blank_line(data, workspace)
        check_r4_view_answers_in_the_shape_asked(data, workspace)
        check_r4_sweep_counts_only_what_changed(data, workspace)
        check_r4_sweep_keeps_a_receipt_when_nothing_is_here(data, workspace)
        check_r4_sweep_does_not_wait_for_another_writer(data, workspace)
        check_r4_a_receipt_holds_numbers_a_clock_can_print(data, workspace)
        check_a_value_holding_a_non_tag_is_only_tag_clears(data, workspace)
        check_a_run_answers_for_the_state_after_the_flush(data, workspace)
        check_an_empty_value_is_none_and_a_comma_is_invalid(data, workspace)
        check_tag_merge_restamps_what_it_renames(data, workspace)
        check_a_tag_write_keeps_what_the_file_carries(data, workspace)
        check_tag_cp_onto_its_source_is_refused(data, workspace)
        check_a_batch_parses_every_line_before_the_first_runs(data, workspace)
        check_a_batch_import_refuses_the_batchs_own_file(data, workspace)
        check_completion_offers_only_what_the_line_takes(data, workspace)
        check_a_dump_row_is_held_to_the_index_shape(data, workspace)
        check_words_off_a_value_stay_one_line(data, workspace)
        check_sweep_leaves_a_value_holding_a_non_tag_to_the_flush(data, workspace)
        check_r5_mv_set_renames_children_first(data, workspace)
        check_r5_rows_under_a_path_follow_only_a_folder(data, workspace)
        check_r5_sweep_sweeps_a_nested_root_once(data, workspace)
        check_r5_a_root_replaced_by_a_link_is_away(data, workspace)
        check_r5_file_verbs_refuse_what_omafiles_keeps(data, workspace)
        check_r5_walk_filters_name_what_they_cannot_read(data, workspace)
        check_r5_a_list_of_names_holds_each_path_once(data, workspace)
        check_r5_an_empty_name_is_quoted(data, workspace)
        check_r5_an_empty_value_is_spelled_not_missing(data, workspace)
        check_r5_a_dangling_destination_parent_is_named_hop_by_hop(data, workspace)
        check_r5_the_prompt_keeps_each_name_on_one_line(data, workspace)
        check_r5_chmod_dry_run_follows_one_inode(data, workspace)
        check_r5_cp_drops_the_sticky_bit(data, workspace)
        check_r5_roots_add_refuses_an_empty_check(data, workspace)
        check_r5_input_behind_an_unnameable_link_is_refused(data, workspace)
        check_r5_a_closed_stdin_is_not_read_by_name(data, workspace)
        check_r5_no_value_where_none_can_be_is_no_refusal(data, workspace)
        check_r5_view_and_roots_subcommands_take_their_own(data, workspace)
        check_r5_the_bash_shim_completes_after_an_equals(data, workspace)
        check_r5_a_flush_writes_only_what_differs(data, workspace)
        check_r5_the_tag_verbs_keep_their_per_file_rules(data, workspace)
        check_r5_completion_offers_what_the_parse_takes(data, workspace)
        check_r5_a_value_is_spelled_once(data, workspace)
        check_r5_a_kept_file_that_will_not_write_says_why(data, workspace)
        check_r5_help_names_no_file_it_does_not_ship(data, workspace)
        check_r6a_a_batch_reads_a_hard_links_pending_copy(data, workspace)
        check_r6a_a_folded_spelling_is_the_stored_name(data, workspace)
        check_r6a_a_filesystem_without_user_xattrs_holds_only_rows(data, workspace)
        check_r6a_a_bind_mount_is_another_filesystem_to_mv(data, workspace)
        check_r6a_a_kept_file_is_known_by_its_inode(data, workspace)
        check_r6a_mv_leaves_a_declared_root_where_it_is(data, workspace)
        check_r6a_a_destination_drops_the_rows_under_it(data, workspace)
        check_r6a_a_move_the_index_would_not_take_is_undone(data, workspace)
        check_r6a_a_drop_the_flush_did_not_make_has_no_receipt(data, workspace)
        check_r6a_an_empty_batch_writes_nothing(data, workspace)
        check_r6a_import_names_a_loop_of_links(data, workspace)
        check_r6_completion_reads_the_parse(data, workspace)
        check_r6_bash_suffixes_only_paths(data, workspace)
        check_r6_zsh_script_runs_when_autoloaded(data, workspace)
        check_r6_a_refusal_spells_what_was_typed(data, workspace)
        check_r6_a_cached_answer_looks_at_nothing(data, workspace)
        check_r6_an_index_answer_asks_search_alone(data, workspace)
        check_r6_a_link_loop_in_a_walk_is_said(data, workspace)
        check_r6_sweep_counts_what_it_left(data, workspace)
        check_r6_a_view_is_named_as_it_is_saved(data, workspace)
        check_r6_recent_says_each_file_once(data, workspace)
        check_r6m_the_file_verbs_count_in_the_parse(data, workspace)
        check_r6m_a_file_verb_spells_what_was_typed(data, workspace)
        check_r7_a_root_at_the_top_keeps_the_whole_path(data, workspace)
        check_r7_a_full_disk_names_the_systems_reason(data, workspace)
        check_r7_an_index_that_may_not_be_written_is_found_first(data, workspace)
        check_r7_a_pipe_on_stdin_is_not_missing(data, workspace)
        check_r7_a_root_now_a_symlink_is_away_where_typed(data, workspace)
        check_r7_a_value_is_split_before_it_is_decoded(data, workspace)
        check_r7_a_fifo_holds_no_value_silently(data, workspace)
        check_r7_the_editor_is_known_by_its_program(data, workspace)
        check_r7_help_names_every_flag_the_parse_takes(data, workspace)
        check_r7_the_bash_shim_offers_each_word_as_one(data, workspace)
        check_r7_completion_reads_a_value_before_the_globals(data, workspace)
        check_r7_no_operand_reads_a_closed_stdin(data, workspace)
        check_r7_a_walk_finds_a_root_turned_link_away(data, workspace)
        check_r7_trash_and_chmod_leave_what_omafiles_keeps(data, workspace)
        check_r7_a_folded_destination_drops_its_stale_row(data, workspace)
        check_r7_an_empty_batch_still_reads_the_index(data, workspace)
        check_r7_a_shut_dir_takes_a_batch_back(data, workspace)
        check_r7_reindex_names_only_rows_it_wrote(data, workspace)
        check_r7_a_refusal_spells_every_path(data, workspace)
        check_r7_a_destination_may_be_the_parent(data, workspace)
        check_r7_a_message_from_the_core_spells_its_paths(data, workspace)
        check_r8_a_rooted_name_may_begin_with_a_byte_order_mark(data, workspace)
        check_r8_mv_and_cp_carry_the_values_tags_into_the_row(data, workspace)
        check_r8_a_dry_run_says_a_stale_row_would_be_dropped(data, workspace)
        check_r8_recent_desktop_orders_within_a_second(data, workspace)
        check_r8_an_href_with_a_query_names_no_file(data, workspace)
        check_r8_a_trailing_newline_is_not_part_of_a_word(data, workspace)
        check_r8_a_mount_point_is_not_moved(data, workspace)
        check_r8_a_walk_gap_does_not_fail_a_later_line(data, workspace)
        check_r8_a_value_in_another_spelling_is_reported(data, workspace)
        check_r8_sweep_counts_a_path_once(data, workspace)
        check_r8_the_sweep_receipt_says_what_is_true(data, workspace)
        check_r8_a_lone_dash_before_the_verb_is_no_flag(data, workspace)
        check_r8_removing_a_nested_root_drops_the_outer_list(data, workspace)
        check_r8_completion_has_no_opinion_on_a_refused_line(data, workspace)
        check_r8_the_editor_is_found_past_env_options(data, workspace)
        check_r8_no_opener_names_both(data, workspace)
        check_r8_a_write_that_changes_nothing_writes_nothing(data, workspace)
        check_r8_a_new_root_waits_for_a_write_that_changes_a_row(data, workspace)
        check_r9_a_recased_row_follows_the_file(data, workspace)
        check_r9_a_recased_root_follows_its_directory(data, workspace)
        check_r9_a_directory_is_told_folding_by_a_name_it_holds(data, workspace)
        check_r9_hard_links_one_case_apart_do_not_fold_a_directory(data, workspace)
        check_r9_a_move_refuses_a_value_it_may_not_read(data, workspace)
        check_r9_a_kept_file_not_made_yet_is_named_as_a_place(data, workspace)
        check_r9_a_link_moves_without_the_row_at_its_name(data, workspace)
        check_r9_an_unset_home_is_named_unset(data, workspace)
        check_r9_a_list_that_will_not_go_is_named(data, workspace)
        check_r9_an_unplugged_mount_point_may_be_declared(data, workspace)
        check_r9_a_batch_names_the_rows_it_took_away(data, workspace)
        check_r9_a_batch_line_escapes_in_double_quotes_as_sh(data, workspace)
        check_r9_the_bash_shim_matches_the_word_as_the_shell_reads_it(data, workspace)
        check_r9_a_batch_calls_a_listing_a_read(data, workspace)
        check_r9_reindex_names_hard_links_tagged_apart(data, workspace)
        check_r9_top_level_refusals_name_the_program(data, workspace)
        check_r9_view_add_names_itself(data, workspace)
        check_r9_tags_says_its_notes_in_the_order_ls_does(data, workspace)
        check_r9_import_says_nothing_of_a_merge_it_refused(data, workspace)
        check_r10_an_alias_the_directory_resolves_is_the_stored_name(data, workspace)
        check_r10_a_list_of_names_finds_a_recased_row(data, workspace)
        check_r10_a_kept_file_not_made_yet_is_refused_in_another_case(data, workspace)
        check_r10_a_walk_names_an_away_root_whose_mount_point_is_gone(data, workspace)
        check_r10_a_batch_names_a_value_it_emptied(data, workspace)
        check_r10_a_row_in_the_other_form_is_the_same_row(data, workspace)
        check_r10_a_dump_names_the_declaration_its_root_needs(data, workspace)
        check_r10_bash_ends_a_folder_past_a_word_break_with_a_slash(data, workspace)
        check_r10_completion_has_no_opinion_past_a_word_that_is_no_global(data, workspace)
        check_r10_a_copy_that_cannot_carry_a_foreign_word_is_refused(data, workspace)
        check_r10_tag_merge_names_a_row_a_link_now_leads_through(data, workspace)
        check_r10_mv_into_a_link_to_itself_is_inside_itself(data, workspace)
        check_r11_prune_takes_the_tree_a_move_left(data, workspace)
        check_r11_a_list_of_names_is_short_where_its_root_is_shut(data, workspace)
        check_r11_roots_migrate_writes_only_a_change(data, workspace)
        check_r11_a_sweep_that_steps_aside_counts_an_away_root_inside(data, workspace)
        check_r11_bash_completes_a_path_attached_to_a_flag(data, workspace)
        check_r11_a_batch_stamps_the_row_it_emptied_with_the_drop(data, workspace)
        check_r11_hard_links_tagged_apart_in_a_batch_agree_with_the_dry_run(data, workspace)
        check_r11_a_destination_under_an_away_root_is_named_for_the_root(data, workspace)
        check_r11_the_state_directory_not_made_yet_is_no_destination(data, workspace)
        check_r11_a_shut_parent_is_the_directory_named(data, workspace)
        check_r11_a_batch_names_a_refused_copy_by_its_line(data, workspace)
        check_r12_import_writes_nothing_through_a_link(data, workspace)
        check_r12_tag_writers_refuse_a_file_omafiles_keeps(data, workspace)
        check_r12_import_refuses_a_row_naming_a_kept_file(data, workspace)
        check_r12_a_walk_skips_the_files_omafiles_keeps(data, workspace)
        check_r12_a_missing_destination_is_on_the_way_to_an_away_root(data, workspace)
        check_r12_a_receipt_count_is_the_integer_json_holds(data, workspace)
        check_r12_sweep_status_prints_the_receipt_shape_only(data, workspace)
        check_r13_trash_drops_the_rows_gio_reports_gone(data, workspace)

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
    # could not be written", and a dump the user typed wrong is not that.
    broken = workspace / "broken.jsonl"
    broken.write_text("not json\n")
    torn = run(data, "import", str(broken))
    assert torn.returncode == 1, torn.returncode
    assert "line 1" in torn.stderr, torn.stderr

    # And the malformed shapes. Each names what is wrong with it rather than the
    # first thing that went missing downstream.
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
    # field leaves the stamp on the row it merges into, so `--since` can still
    # judge that file.
    stamped_row = json.loads(run(data, "--json", "json", "--", str(subject)).stdout)
    assert stamped_row["tagged"] > 0
    broken.write_text(json.dumps({"path": str(subject.resolve()),
                                  "tags": stamped_row["tags"]}) + "\n")
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
    # A room of its own: `verify` and `reindex` over the whole workspace would
    # answer for, and rewrite, every other check's files.
    room = workspace / "truth"
    room.mkdir()
    subject = room / "truth.txt"
    subject.write_text("t")
    run(data, "tag", "add", "--tag", "indexed", "--", str(subject))

    clean = run(data, "verify", str(room))
    assert clean.returncode == 0, clean.stdout + clean.stderr

    os.setxattr(subject, "user.omafiles.tags", b"ondisk")
    drifted = run(data, "verify", str(room))
    assert drifted.returncode == 1
    assert "xattr-differs" in drifted.stdout

    # Without the flag the index keeps its answer -- and makes that true on the
    # disk, so the disagreement is gone and a second `verify` has nothing to
    # report.
    settled = run(data, "reindex", str(room))
    assert settled.returncode == 0, settled.stderr
    assert str(subject.resolve()) in settled.stdout, "reindex changed a row and said nothing"
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["indexed"]
    assert os.getxattr(subject, "user.omafiles.tags").decode() == "indexed"
    assert run(data, "verify", str(room)).returncode == 0, "reindex left them apart"

    # With the flag, the file's own tags win instead.
    os.setxattr(subject, "user.omafiles.tags", b"ondisk")
    reindexed = run(data, "reindex", "--from-xattr", str(room))
    assert reindexed.returncode == 0, reindexed.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["ondisk"]
    assert run(data, "verify", str(room)).returncode == 0

    # A file the index never knew about is adopted from its xattr.
    stray = room / "stray.txt"
    stray.write_text("s")
    os.setxattr(stray, "user.omafiles.tags", b"found")
    run(data, "reindex", str(room))
    assert run(data, "tag", "ls", "--", str(stray)).stdout.split() == ["found"]


def check_tag_cp_adds(data, workspace):
    """`tag cp` adds tags to the destination; it never takes any away, so
    copying from a file with no tags leaves the target's tags and row alone."""
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

    # `--json has` answers in JSON as well as in the exit code.
    for tag, expected, code in (("present", True, 0), ("absent", False, 1)):
        spoken = run(data, "--json", "has", "--tag", tag, "--", str(subject))
        assert spoken.returncode == code, (tag, spoken.returncode)
        answer = json.loads(spoken.stdout)
        assert answer["has"] is expected and answer["tag"] == tag
        assert answer["path"] == str(subject.resolve())

    # `omafiles <verb> --help` prints that verb's page, not the whole screen.
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

    # Scoped rename describes its collection-wide form through tag merge.
    page = run(data, "tag", "rename", "--help")
    assert page.returncode == 0, page.stderr
    assert "tag merge" in page.stdout, page.stdout
    assert "tag clear" not in page.stdout, page.stdout

    # And a miss names what was actually asked about -- `tag rename`, not `tag`,
    # which is a command that exists.
    missed = run(data, "tag", "nonsense", "--help")
    assert missed.returncode == 1
    assert "unknown command: tag nonsense" in missed.stderr, missed.stderr

    # A word that is not a verb is a typo, and `--help` after it does not make it
    # one: both spellings say the same thing, on stderr with nothing on stdout.
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
        # And the file's own copy: a committed batch writes the xattr, which the
        # rollback case, asserting its absence, cannot show.
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
    # shell name would otherwise be offered, as cargo, clap and cobra all do.
    for words in (["tag"], ["completions"], ["tag", "merge"]):
        dashed = run(data, "__complete", "--", *words, "-")
        assert dashed.returncode == 0, dashed.stderr
        offered = dashed.stdout.split()
        assert offered, f"{words} offered nothing for a bare -"
        assert all(word.startswith("-") for word in offered), (words, offered)
        assert "--quiet" in offered, (words, offered)
        # Every verb but the one that refuses --json: completion does not
        # offer an error message.
        assert ("--json" in offered) == (words != ["completions"]), (words, offered)

    # And what is offered is what the subcommand will take: no `--tag` for
    # `tag clear`, which the per-subcommand gate refuses. Asked of the gate
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

    # A shell script has no JSON shape, which is what the README says. A global
    # that one verb does not have is refused by that verb.
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
    # every `export` here would read an empty one.
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
    """The ls filters.

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
    # all do, as the declaration -- `arg "[dir]"`, optional -- says.
    here = run(data, "ls", "--untagged", cwd=room)
    assert here.returncode == 0, here.stderr
    assert str(bare.resolve()) in here.stdout.splitlines(), here.stdout
    untagged = run(data, "ls", "--untagged", str(room)).stdout.splitlines()
    assert str(bare.resolve()) in untagged
    assert str(ods.resolve()) not in untagged

    # The same entries verify and reindex walk: a dotfile and a directory are
    # both things somebody can tag, so both are things that can be untagged.
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

    # A symlink is an entry of its own, listed by its own name, as `find -name`
    # lists it; `check_a_walk_lists_what_the_disk_holds` pins the rest.
    twinned = room / "twinned"
    twinned.mkdir()
    original = twinned / "once.txt"
    original.write_text("o")
    os.symlink("once.txt", twinned / "twice.txt")
    listed = run(data, "ls", "--untagged", str(twinned)).stdout.splitlines()
    assert listed == [str(original.resolve()), str(twinned.resolve() / "twice.txt")], listed

    # The contradictions are reported before the missing directory, and each
    # names the flag that was actually typed.
    for flag in ("--tag", "--any"):
        conflicted = run(data, "ls", "--untagged", flag, "work")
        assert conflicted.returncode == 1
        assert "opposite questions" in conflicted.stderr, conflicted.stderr
        assert flag in conflicted.stderr, (flag, conflicted.stderr)
        assert "needs a directory" not in conflicted.stderr, conflicted.stderr

    # A directory is not an answer to a question about file extensions, and it
    # is not one on either side of the flag either: the untagged walk and the
    # index scan both leave folders out of `--ext`.
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

    # A leading dot names the file, it does not introduce an extension, as
    # Python's `os.path.splitext('.cshrc')` and Go (golang/go#24239) agree.
    # All four shapes.
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

    # A directory nobody may open is not an empty directory, and `ls
    # --untagged` says so as `verify` does. On stderr, not among the answers.
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
        # ones under it get: /usr/bin/ls says `cannot open directory` and
        # /usr/bin/find `Permission denied`; neither calls it empty.
        named = run(data, "ls", "--untagged", str(shut))
        assert "unreadable" in named.stderr, named.stderr
        assert str(shut.resolve()) in named.stderr, named.stderr
        assert named.stdout.strip() == "", named.stdout
        # Exit 1, like the descendant one line up: a list one row short is not
        # the answer that was asked for -- see
        # check_an_incomplete_answer_is_not_a_success.
        assert named.returncode == 1, named.stderr
        assert run(data, "--quiet", "ls", "--untagged", str(shut)).returncode == 1
        blind_status = run(data, "ls", "--untagged", str(room))
        assert blind_status.returncode == 1, blind_status.stderr
    finally:
        shut.chmod(0o755)

    # --since counts from the tag stamp. A row without one cannot be judged, and
    # ls says so on stderr rather than returning a short list that looks whole.
    index = data / "omafiles/index.jsonl"
    stampless = room / "stampless.txt"
    stampless.write_text("l")
    with index.open("a") as handle:
        handle.write(json.dumps({
            "path": str(stampless.resolve()), "tags": ["flag-a"],
            }) + "\n")

    recent_only = run(data, "ls", "--tag", "flag-a", "--since", "1h")
    assert str(ods.resolve()) in recent_only.stdout
    assert str(stampless.resolve()) not in recent_only.stdout
    assert "carry no tagged stamp" in recent_only.stderr

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

    # A duration too big to hold is refused, with a message, not folded into
    # one that fits.
    for huge in ("99999999999999999999d", "999999999999999d", "9223372036854775807d"):
        refused = run(data, "ls", "--tag", "flag-a", "--since", huge)
        assert refused.returncode == 1, (huge, refused.stdout)
        assert refused.stdout.strip() == "", (huge, refused.stdout)
        assert "invalid duration" in refused.stderr, (huge, refused.stderr)
    # The largest window that does fit is still a window, not a rejection.
    assert run(data, "ls", "--tag", "flag-a", "--since", "106751991167d").returncode == 0

    # And an empty one is refused rather than dropped, as `--tag ""` and `--any
    # ""` are: `ls --since "$WINDOW"` with WINDOW unset is not the whole index.
    blank = run(data, "ls", "--since", "")
    assert blank.returncode == 1, blank.stdout
    assert blank.stdout.strip() == "", blank.stdout
    assert "invalid duration" in blank.stderr, blank.stderr


def check_a_failed_write_is_not_a_success(data, workspace):
    """stdout that could not be written is reported, whichever verb wrote it.

    As coreutils' `close_stdout`: the last write lands in the buffer and the
    *flush* is what fails, so the check is after the flush at exit, once, for
    every verb."""
    room = workspace / "fullness"
    room.mkdir()
    subject = room / "w.txt"
    subject.write_text("w")
    run(data, "tag", "add", "--tag", "written", "--", str(subject))

    full = Path("/dev/full")
    if not full.exists():
        return
    env = cli_env(data)
    # And the verb that was writing is named. Every other diagnostic here
    # carries it -- `ls: --untagged and --tag ask opposite questions` -- and
    # coreutils prints `ls: write error: ...` for this very failure, so a script
    # reading a log of a dozen commands can tell which one lost its answer.
    for args, verb in ((["ls", "--tag", "written"], "ls"),
                       (["--json", "ls", "--tag", "written"], "ls"),
                       (["tags"], "tags"),
                       (["export"], "export"),
                       (["recent"], "recent"),
                       (["tag", "ls", "--", str(subject)], "tag ls"),
                       (["--version"], "omafiles"),
                       (["--help"], "omafiles")):
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
    assert "no tagged stamp" in loud.stderr and hushed.stderr.strip() == ""
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
    """mv and trash on a directory take the rows inside it with them."""
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

    # An untagged row has no tag time to compare, so --since beside --untagged
    # is refused rather than answered with an empty list.
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

    # Under `--json` each prints one document and nothing before it.
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

    # And cp stamps the copy's own xattr, through the same commit as
    # everything else.
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
    the index never heard of, as `export` and `has` say."""
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

    Not "all forty succeed": under enough contention a writer may give up
    waiting for the lock. A writer either succeeds and its tag is there, or
    fails loudly."""
    room = workspace / "concurrent"
    room.mkdir()
    env = cli_env(data)

    def start(jobs):
        return [
            (tag, path, subprocess.Popen(
                [str(CLI), "tag", "add", "--tag", tag, "--", str(path)],
                cwd=ROOT, env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))
            for tag, path in jobs
        ]

    def settle(running):
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

    outcome = settle(start([(f"race{i}", f) for i, f in enumerate(files)]))
    tags_now = run(data, "export").stdout
    for tag, path, code, err in outcome:
        if code == 0:
            assert str(path.resolve()) in tags_now, f"{path.name} exited 0 and is not indexed"
        else:
            # Giving up on the lock is allowed; doing so quietly is not.
            assert err.strip(), f"{path.name} failed with nothing on stderr"

    survived = sum(1 for _, _, code, _ in outcome if code == 0)
    assert survived >= 2, "no concurrent write got through at all"

    # Writers that must succeed outright. The forty above assert only that
    # nothing is lost silently, which stays true if the lock timeout is cut to a
    # millisecond and most writers give up. Three writers held off for a second
    # and a half are still waiting then, and all land once it is let go.
    modest = []
    for i in range(3):
        f = room / f"modest{i}.txt"
        f.write_text("x")
        modest.append(f)
    jobs = [(f"modest{i}", f) for i, f in enumerate(modest)]
    lock = forge_lock(data)
    try:
        waiting = start(jobs)
        time.sleep(1.5)
        assert all(process.poll() is None for _, _, process in waiting), \
            "a writer stopped waiting for a lock held a second and a half"
    finally:
        lock.unlink(missing_ok=True)
    for tag, path, code, err in settle(waiting):
        assert code == 0, f"{path.name} could not take the lock in time: {err}"

    # The same file from many processes accumulates rather than overwrites.
    shared = room / "shared.txt"
    shared.write_text("s")
    outcome = settle(start([(f"heap{i}", shared) for i in range(20)]))
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

    # A directory carrying tags the index never heard of: `tag add -- somedir`
    # tags directories, so the walk sees them.
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

    # The directory you actually named, which QDirIterator never yields.
    named = room / "named"
    named.mkdir()
    os.setxattr(named, "user.omafiles.tags", b"itsown")
    walked = run(data, "verify", str(named))
    assert walked.returncode == 1, walked.stdout
    assert str(named.resolve()) in walked.stdout, walked.stdout
    assert run(data, "reindex", str(named)).returncode == 0
    assert run(data, "tag", "ls", "--", str(named)).stdout.split() == ["itsown"]

    # A symlink and its target are one file. Both turn up in a walk, which
    # reports the problem once and writes one row.
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

    # A symlink pointing out of the tree is not part of the tree, so `verify`
    # does not report the file at its far end and `reindex` writes no row there.
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
    # `ls` lists the link itself, by its own name, and never the file beyond.
    assert run(data, "ls", "--untagged", str(escape)).stdout.splitlines() == \
        [str(escape.resolve() / "out.txt")]

    # A directory nobody may open is not an empty directory: silence from a verb
    # whose job is to say whether two things agree reads as agreement.
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

    # And a single file nobody may read. Same distinction, one level down: an
    # xattr that could not be read is not an xattr that is absent.
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
                          env=cli_env(data),
                          text=True, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    dotted = subprocess.run([str(CLI), "verify", "."], cwd=room / "here",
                            env=cli_env(data),
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
    This is the recovery, asserted rather than assumed: it is why no inode is
    stored to detect the move."""
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
    """import stores a dump path through a symlinked parent as the dump gives
    it: a row whose path leads through a link is missing, and nothing is written
    to the file the link leads to."""
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
    assert str(room / "through" / "g.txt") in rows[0], rows[0]
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == []
    assert run(data, "verify", str(room)).stdout.startswith("missing\t"), rows

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
    """The file's xattr is written after the commit, never before, so a commit
    that fails -- a read-only index is the easiest way to see it -- leaves no
    file claiming a tag no row recorded."""
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

    # The repair has to work. A tag set that does not change still rewrites the
    # xattr, so a file whose own copy has gone missing -- a `cp` without
    # --preserve=xattr -- is put right by `tag add`.
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

    # `tag rm` is the same shape: removing a tag the file never had changes no
    # tag, but it still rewrites the file's own copy.
    os.removexattr(subject, "user.omafiles.tags")
    removed = run(data, "tag", "rm", "--tag", "neverthere", "--", str(subject))
    assert removed.returncode == 0, removed.stderr
    assert os.getxattr(subject, "user.omafiles.tags").decode() == "landed"
    assert json.loads(run(data, "--json", "json", "--", str(subject)).stdout)["tagged"] == stamp

    # With row and copy already agreeing, neither changes anything, and a
    # receipt names only what changed.
    for verb in (("tag", "add", "--tag", "landed"), ("tag", "rm", "--tag", "neverthere")):
        spoken = run(data, "--json", *verb, "--", str(subject))
        assert json.loads(spoken.stdout) == [], (verb, spoken.stdout)


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
    # `reindex` do. `--all` is the whole index, spelled out.
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
                              env=cli_env(data),
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

    # A flag is only accepted by a verb that has it.
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

    `recent` is ordered by when a path was last tagged, so a write that changes
    no tag leaves the stamp alone."""
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

    # `tag add` refuses these, and so does import: `tag rm` normalises before
    # comparing and could never match them. Case is not among them: `tag add
    # --tag UPPER` lowercases rather than refusing, and import has to agree.
    for bad in (["Has Space!"], ["-lead"], ["a" * 65]):
        refused = run(data, "import", str(dump(bad)))
        assert refused.returncode == 1, bad
        assert "invalid tag" in refused.stderr, bad

    # A comma is the xattr's separator, so a tag holding one is refused.
    comma = run(data, "import", str(dump(["red,green"])))
    assert comma.returncode == 1
    assert "invalid tag" in comma.stderr

    # Nothing was written by any of them.
    assert run(data, "tag", "ls", "--", str(subject)).stdout.strip() == ""

    # `-` is stdin, the way it is for `batch`. `export` writes the dump to
    # stdout and this reads one, so the pair is a pipe.
    piped = run(data, "import", "-", stdin=dump(["piped"]).read_text())
    assert piped.returncode == 0, piped.stderr
    assert "piped" in run(data, "tag", "ls", "--", str(subject)).stdout.split()
    run(data, "tag", "clear", "--", str(subject))

    good = run(data, "import", str(dump(["fine", "Also-Fine"])))
    assert good.returncode == 0, good.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["also-fine", "fine"]

    # And what it stored is what the xattr carries, so a reindex agrees with it.
    assert os.getxattr(subject, "user.omafiles.tags") == b"also-fine,fine"

    # A malformed `tagged` is told apart from an absent one, the way `path` and
    # `tags` are: a string or `true` is refused, not read as 0 or 1.
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

    # An absent one is still absent, not a refusal.
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
        if "no tagged stamp" not in note:
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
        # `tag` and `roots` are the two verbs with subcommands of their own, so
        # what a reader reads there is two words and not one.
        found.add(f"{words[0]} {words[1]}"
                  if words[0] in ("tag", "roots", "view") and len(words) > 1
                  else words[0])
    return found


def check_one_version(data, workspace):
    """Every place that says a version says the same one.

    The binaries share a generated header; the usage declaration, the CLI
    reference, the package and the release manifest are literals. release-please
    moves only the lines marked `x-release-please-version`, so every copy has to
    carry the mark and agree with `--version`."""
    spoken = run(data, "--version")
    assert spoken.returncode == 0, spoken.stderr
    version = spoken.stdout.split()[-1]
    assert re.fullmatch(r"\d+\.\d+\.\d+", version), spoken.stdout

    copies = {"qmake/version.pri": r"^OMAFILES_VERSION = (\S+) # x-release-please-version$",
              "omafiles.usage.kdl": r'^version "([^"]+)" // x-release-please-version$',
              "docs/cli.md": r"^- \*\*Version:\*\* (\S+) <!-- x-release-please-version -->$",
              "pkgbuild/PKGBUILD": r"^pkgver=(\S+) # x-release-please-version$"}
    for path, pattern in copies.items():
        declared = re.search(pattern, (ROOT / path).read_text(), re.M)
        assert declared, f"{path} has no version marked for release-please"
        assert declared.group(1) == version, (path, declared.group(1), version)
    manifest = json.loads((ROOT / ".release-please-manifest.json").read_text())
    assert manifest == {".": version}, manifest

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

    # The prompt counts rows, not arguments: `trash somedir` names one path and
    # drops every row beneath it.
    sub = room / "sub"
    sub.mkdir()
    (sub / "deep.txt").write_text("d")
    run(data, "tag", "add", "--tag", "counted", "--", str(sub), str(sub / "deep.txt"))
    refused = run(data, "trash", "--", str(sub))
    assert refused.returncode == 1
    assert "2 index row(s)" in refused.stderr, refused.stderr

    # The one failure every machine can be made to reproduce: no gio that
    # runs. Its message says which.
    (room / "gone.txt").write_text("g")
    if (shut := shut_gio()) is not None:
        prefix, said = shut
        without = run(data, "--yes", "trash", "--", str(room / "gone.txt"), wrap=prefix)
        assert without.returncode != 0
        assert said in without.stderr, without.stderr
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

    POSIX `unlink()` removes the link; coreutils `rm` and `mv` and `gio trash`
    all act on the link, and so do these."""
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

    # A dangling link is a path that is there, though `exists()` follows it and
    # says no.
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

    Refusing is the rule these verbs apply to a regular file that is there, and
    coreutils will not write through a dangling link either: `cp` says so in as
    many words."""
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

    # The three answers still agree.
    assert run(data, "verify", str(room)).returncode == 0

    assert run(data, "cp", str(subject), str(dang)).returncode == 1
    assert not (room / "gone.txt").exists(), "cp wrote through the dangling link"

    # A link that is not dangling is refused as well.
    live = room / "live"
    live.symlink_to(subject)
    assert run(data, "mv", str(subject), str(live)).returncode == 1


def check_the_prompt_names_the_paths(data, workspace):
    """The question says which paths, not just how many.

    `rm: 1 path(s). Continue?` is the same sentence whether the path is the one
    you typed or the target a symlink led to; `rm -i` names the file --
    `remove regular file 'a.txt'?`. The row count stays beside it, because one
    path can carry a tree of rows."""
    room = workspace / "asking"
    room.mkdir()
    named = room / "named.txt"
    named.write_text("n")

    refused = run(data, "rm", str(named))
    assert refused.returncode == 1
    assert str(named.resolve()) in refused.stderr, refused.stderr
    assert named.exists()

    # And the row count is there too: `trash somedir` names one path and drops
    # every row beneath it.
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

    One shape in a dry run, a real one, and for a file with no tags, with the
    destination in all three -- as `rm` and `trash` say `{"removed":[...]}` in
    both modes."""
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

    # An untagged file has no rows, and the destination is still the answer.
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

    A directory somebody chmod'd shut is not deletion, and on a filesystem that
    does not carry user.* xattrs the index is the only copy of those tags. Only
    ENOENT is absence here. TMSU's repair steps over a permission error and only
    forgets a file that is genuinely not there."""
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

    # A row whose file really is gone is still dropped.
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

    A file the index side reports is not reported again by the disk walk, and
    an indexed shut directory is one `unreadable`, not that and an
    `xattr-unreadable` besides."""
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

    An xattr set by hand that the tag rules reject is a state some verb has to
    clear, and verify names the one that does."""
    room = workspace / "settling"
    room.mkdir()

    # What can be salvaged is salvaged. All-or-nothing is the right rule for a
    # list somebody typed and the wrong one for a value read off a file:
    # throwing `good` away along with `Bad Tag` left nothing to index at all.
    # The value itself is only `tag clear`'s to rewrite, so it stays, and the
    # run that promised the two agree exits 1 naming that command.
    part = room / "part.txt"
    part.write_text("p")
    os.setxattr(part, "user.omafiles.tags", b"good,Bad Tag")
    rebuilt = run(data, "reindex", str(room))
    assert rebuilt.returncode == 1, rebuilt.stderr
    assert "Bad Tag" in rebuilt.stderr and "tag clear" in rebuilt.stderr, rebuilt.stderr
    assert run(data, "tag", "ls", "--", str(part)).stdout.split() == ["good"]
    assert os.getxattr(part, "user.omafiles.tags") == b"good,Bad Tag"
    rows = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
    assert [row["problem"] for row in rows if row["path"] == str(part.resolve())] \
        == ["xattr-invalid"], rows

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
    assert run(data, "tag", "clear", "--", str(bad), str(part)).returncode == 0
    assert run(data, "reindex", str(room)).returncode == 0
    assert run(data, "verify", str(room)).returncode == 0

    # With a row beside it the value is still only `tag clear`'s: plain reindex
    # lets the index win over valid tags, never over a word that is not one.
    assert run(data, "tag", "add", "--tag", "good", "--", str(part)).returncode == 0
    for value in (b"Has Space!", b"good,Bad Tag"):
        for spelling in (("reindex", str(room)), ("reindex", "--from-xattr", str(room))):
            os.setxattr(part, "user.omafiles.tags", value)
            again = run(data, *spelling)
            assert again.returncode == 1 and "tag clear" in again.stderr, \
                (spelling, again.stderr)
            assert os.getxattr(part, "user.omafiles.tags") == value, spelling
            rows = json.loads(run(data, "--json", "verify", str(room)).stdout)["problems"]
            mine = [row for row in rows if row["path"] == str(part.resolve())]
            assert [row["problem"] for row in mine] == ["xattr-invalid"], (spelling, mine)
            assert mine[0]["detail"].endswith("drops the value and the row"), mine
    os.removexattr(part, "user.omafiles.tags")

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

    `--quiet reindex` prints the rows it wrote, as `--json --quiet` does, and
    prune, import and batch all answer whatever --quiet says."""
    room = workspace / "quietly"
    room.mkdir()
    subject = room / "q.txt"
    subject.write_text("q")
    os.setxattr(subject, "user.omafiles.tags", b"quietly")
    hushed = run(data, "--quiet", "reindex", str(room))
    assert hushed.returncode == 0, hushed.stderr
    assert hushed.stdout.splitlines() == [str(subject.resolve())], hushed.stdout

    # And it keeps the errors: a value left as it is, on a path the run
    # promised to settle, is why it exits 1.
    noisy = room / "noisy.txt"
    noisy.write_text("n")
    os.setxattr(noisy, "user.omafiles.tags", b"kept,Bad Tag")
    quiet = run(data, "--quiet", "reindex", str(room))
    assert quiet.stdout.splitlines() == [str(noisy.resolve())], quiet.stdout
    assert quiet.returncode == 1 and "left as it is" in quiet.stderr, quiet.stderr


def check_each_tag_subcommand_takes_its_own_flags(data, workspace):
    """Each `tag` subcommand has its own flags, not the whole family's.

    A sibling's flag is refused, not accepted and ignored: `tag clear --tag
    drop` is a restricting flag on an unrestricted destructive write."""
    room = workspace / "tagflags"
    room.mkdir()
    subject = room / "s.txt"
    subject.write_text("s")
    other = room / "o.txt"
    other.write_text("o")

    # The matrix as omafiles.usage.kdl draws it: -t/--tag on
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

    # `tag clear --tag` is refused, so both tags stay, and the row with them.
    run(data, "tag", "add", "--tag", "drop", "--tag", "keep", "--", str(subject))
    refused = run(data, "tag", "clear", "--tag", "drop", "--", str(subject))
    assert refused.returncode == 1, refused.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["drop", "keep"]

    # And `tag ls --tag base` is refused rather than printing every tag while
    # appearing to honour a filter.
    filtered = run(data, "tag", "ls", "--tag", "drop", "--", str(subject))
    assert filtered.returncode == 1
    assert filtered.stdout == "", filtered.stdout

    # --from takes the next word, so on a verb without it, the refusal names
    # --from and not the flag that followed.
    swallowed = run(data, "tag", "add", "--from", "--tag", "work", "--", str(subject))
    assert swallowed.returncode == 1
    assert "unexpected --from" in swallowed.stderr, swallowed.stderr

    # Nor does it take the `--`, which keeps the README's promise about odd
    # names: "Flags come first and paths after --, so -weird.pdf is a path and
    # not a flag."
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

    A row that cannot be stat'ed where the file landed -- under a directory
    nobody may open, or a dangling link -- still moves with the folder, so the
    dry run, which rewrites the path and asks nothing, promises no more than the
    real run delivers."""
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

    # The row under the shut directory is in the index under its new name, so
    # the file is not one the index has never heard of.
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

    `ls --ext -j sub` is an extension of `-j` under `sub`, not --json. POSIX
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
    # `-y` is the value of `--from`, not `--yes`, and the `--` is not eaten.
    assert copied.returncode == 0, copied.stderr
    assert run(data, "tag", "ls", "--", str(target)).stdout.split() == ["odd"]

    # The globals still work on both sides of the verb, and out of the
    # environment.
    assert json.loads(run(data, "--json", "ls", "--tag", "optval").stdout)
    assert json.loads(run(data, "ls", "--tag", "optval", "--json").stdout)
    assert json.loads(run(data, "ls", "--tag", "optval",
                          extra_env={"OMAFILES_JSON": "1"}).stdout)


def check_an_undated_row_is_not_stamped_by_nothing(data, workspace):
    """A write that changes no tag does not stamp a row that has no stamp.

    `tag rm --tag` naming a tag the file never had leaves an undated row
    undated rather than sending it to the top of `recent`. The README says of
    those rows that they are stamped the next time their tags change."""
    room = workspace / "undated"
    room.mkdir()
    subject = room / "g.txt"
    subject.write_text("g")
    source = room / "src.txt"
    source.write_text("s")
    run(data, "tag", "add", "--tag", "unstamped", "--", str(source))

    # An undated row: no `tagged` key.
    index = data / "omafiles/index.jsonl"
    with index.open("a") as handle:
        handle.write(json.dumps({"path": str(subject.resolve()),
                                 "tags": ["unstamped"]}) + "\n")

    def row():
        return json.loads(run(data, "--json", "json", "--", str(subject)).stdout)

    assert "tagged" not in row(), row()
    for changes_nothing in (("tag", "rm", "--tag", "notpresent", "--", str(subject)),
                            ("tag", "add", "--tag", "unstamped", "--", str(subject)),
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

    Exit 0 there would leave the disagreement `verify` fails on --
    `reindex && verify` with no fixed point -- and the error survives --quiet."""
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

    # And once the permission is what it has to be, the pair converges.
    assert run(data, "reindex", str(room)).returncode == 0
    assert run(data, "verify", str(room)).returncode == 0
    assert os.getxattr(subject, "user.omafiles.tags") == b"want"


def check_a_dry_run_refuses_what_the_real_run_refuses(data, workspace):
    """--dry-run is "print the write, do not commit", not "print a write".

    `rm`, `cp` and `mv` each refuse in a dry run what the real run turns down,
    since none of the three needs the work done to know the answer -- a dry run
    naming tags as about to go for a command that will not touch them is worse
    than none."""
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
    always means the value was dropped, not that somebody cleared the tags. The
    nonzero exit keeps `reindex --from-xattr && verify` from reading as a
    success the verify then contradicts."""
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
    # path walked away from, so it is the same nonzero exit.
    os.setxattr(subject, "user.omafiles.tags", b"Has Space!")
    unusable = run(data, "reindex", "--from-xattr", str(room))
    assert unusable.returncode == 1, unusable.stderr
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["alpha"]
    assert "tag clear" in unusable.stderr, unusable.stderr
    plain = run(data, "reindex", str(room))
    assert plain.returncode == 1 and "tag clear" in plain.stderr, plain.stderr
    assert os.getxattr(subject, "user.omafiles.tags") == b"Has Space!"


def check_a_shut_directory_is_not_an_empty_index(data, workspace):
    """The three read verbs say what they could not look at, and list the rest.

    A directory somebody chmod'd shut is not deletion. Skipping the row is right
    -- nothing can list what the system will not describe -- but it is said out
    loud, on stderr, as `ls --untagged` says it."""
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

    On a writable file that write is a ctime bump for nothing; on a file the
    filesystem will not take a write for, it would fail over a tree `verify`
    calls clean."""
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
    os.setxattr(subject, "user.omafiles.tags", b"Kept,kept")
    salvaged = run(data, "reindex", "--from-xattr", str(room))
    assert salvaged.returncode == 0, salvaged.stderr
    assert os.getxattr(subject, "user.omafiles.tags") == b"kept"
    assert run(data, "verify", str(room)).returncode == 0


def check_a_batch_prints_nothing_until_it_commits(data, workspace):
    """A reindex line inside a batch does not announce rows a later line unwrites.

    "A batch prints nothing until it commits": `reindex` defers its rebuilt
    paths the way `prune` defers its dropped list, so a later failing line
    leaves no path on stdout naming a row that was rolled back."""
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

    # A batch that commits still does the work, and announces it only after the
    # commit, as every other write verb inside a batch does.
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

    Whether a name is a directory is a pure precondition -- it is one or it is
    not, whatever the run does next -- so `rm g1.txt somedir g3.txt` refuses
    before it asks, as it does for `path missing`.

    `trash` has the same shape with `gio`: whether it is on PATH takes no work
    to find out, so `--dry-run trash G` says `gio is not installed` as the real
    run does."""
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
    # the work.
    dry = run(data, "--dry-run", "rm", "--", str(first), str(blocker), str(last))
    assert dry.returncode == 1, dry.stdout
    assert dry.stdout.strip() == "", dry.stdout

    # trash: gio is a lookup on PATH, so both modes answer the same way.
    tree = room / "G"
    (tree / "s").mkdir(parents=True)
    (tree / "x").write_text("x")
    (tree / "s/y").write_text("y")
    run(data, "tag", "add", "--tag", "asked-first", "--",
        str(tree), str(tree / "x"), str(tree / "s/y"))
    if (shut := shut_gio()) is None:
        return
    prefix, said = shut
    real = run(data, "--yes", "trash", str(tree), wrap=prefix)
    assert real.returncode == 1 and said in real.stderr, real.stderr
    for shape in (("--dry-run",), ("--json", "--dry-run")):
        blind = run(data, *shape, "trash", str(tree), wrap=prefix)
        assert blind.returncode == real.returncode, (shape, blind.returncode)
        assert said in blind.stderr, (shape, blind.stderr)
        assert blind.stdout.strip() == "", (shape, blind.stdout)
    # Nor is it asked about, for the same reason.
    status, seen = run_pty(data, "trash", str(tree), wrap=prefix)
    assert status == 1 and "Continue?" not in seen, seen
    assert tree.exists()


def check_every_operand_is_tried(data, workspace):
    """rm and trash do not abandon the tail of the list in silence.

    `/usr/bin/rm y1.txt cdir y3.txt` unlinks both files it can and reports the
    one it cannot, exit 1; `chmod` and `chown` report every operand they walked
    away from too, and so does `rm z1.txt noperm/z2.txt z3.txt` here.
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

    # A recorder in place of every opener, so nothing on this machine's
    # desktop is ever started and the launch is a line in a log.
    log = room / "opened.log"
    only_fake = recording_openers(log)
    if only_fake is None:
        return

    missed = run(data, "open", str(first), str(room / "nosuch"), str(last),
                 wrap=only_fake)
    assert missed.returncode == 2, missed.stderr
    assert not log.exists(), "open launched the head of a list it was going to refuse"
    # Nothing half-said on stdout either: there is no half to report.
    assert missed.stdout.strip() == "", missed.stdout
    empty = run(data, "--json", "open", str(first), str(room / "nosuch"),
                wrap=only_fake)
    assert empty.returncode == 2 and empty.stdout.strip() == "", empty.stdout

    # And with every operand there, every operand is launched.
    both = run(data, "open", str(first), str(last), wrap=only_fake)
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
    """The exit table's worked example holds for a `[dir]` too.

    A row the system will not describe is exit 1 -- `ls` and `tags` skip it,
    `verify` calls it `unreadable` -- and every operand of `rm`, `mv` and the
    tag verbs answers 1 the same way. So does the directory a query is scoped
    to, the dump `import` and `batch` read, and the parent `mv` and `cp` are
    asked to land a file in: `Permission denied` is 1, not the 2 of a path
    that is not there."""
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
    # names inside it and cannot stat one of them, which is a shortfall it has
    # to report.
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

    `stat` still answers for a chmod 000 tree the process owns, so `ls`, `tags`
    and `prune`, which do not walk the disk, ask whether it opens rather than
    answer as if the folder were simply empty."""
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
    """--quiet is "No stderr except errors", and this is not an error.

    A filesystem that refuses to mirror the tags onto the file is `tag add`'s
    documented exit 0 -- the row is the record -- so `--quiet tag add` on a
    chmod 444 file prints nothing on stderr. Where the same refusal is why the
    run exits 1 -- `reindex`, and `tag clear` on a path with no row -- it is the
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

    # The other half: where the refused mirror is the reason for the
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
    never there gets. The sentence names the target, the end that has to be
    created or repointed, since `path missing: dang` is false about a link
    `ls -l` lists and `rm` removes."""
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

    # A name that really was never there says `path missing`: the two are the
    # same status and not the same diagnosis.
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
    stderr and exit 1, so `--quiet`, which takes the note away, still leaves a
    script a failing status. `ls --untagged` over a directory nobody may open is
    the same. /usr/bin/ls exits 2 and find exits 1 in that situation, and the
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
    assert "carry no tagged stamp" in judged.stderr, judged.stderr


def check_a_usage_line_carries_the_usage(data, workspace):
    """The `Usage:` line carries the arguments, not just the verb.

    `prune` is where it shows most, because both of its arguments are
    optional."""
    expected = {
        "prune": "Usage: omafiles prune [dir] [--all]",
        "verify": "Usage: omafiles verify [dir]",
        "mv": "Usage: omafiles mv <old> <new> [--force] [--no-clobber] [--interactive] "
              "[--keep-both] [--backup] [--no-target-directory]",
        "recent": "Usage: omafiles recent [--desktop] [n]",
        "has": "Usage: omafiles has --tag <tag> -- <path>",
        "reindex": "Usage: omafiles reindex [--from-xattr] [dir]",
        "completions": "Usage: omafiles completions <bash|zsh|fish>",
        # `export` takes no arguments, so its first gap is its description and
        # not its shape.
        "export": "Usage: omafiles export",
    }
    for verb, line in expected.items():
        helped = run(data, verb, "--help")
        assert helped.returncode == 0, (verb, helped.stderr)
        assert helped.stdout.splitlines()[0] == line, (verb, helped.stdout)

    # Two-word commands keep their full synopsis.
    for verb, line in (("tag add", "Usage: omafiles tag add --tag <tag>... -- <path>..."),
                       ("tag cp", "Usage: omafiles tag cp --from <path> -- <path>..."),
                       ("tag rename", "Usage: omafiles tag rename <from> <to> [-- <path>...]")):
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

    Each of these is a decision rather than a bug, so README.md, docs/cli.md or
    --help has to say it is meant."""
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
        assert "conflict flags:" in helped and "--keep-both" in helped, verb
    assert "occupied destination by default" in readme
    assert reference.count("An occupied destination is refused unless a conflict flag is given") == 2, "mv or cp says nothing"

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
    `path missing` keeps, so the two foreseeable failures agree.

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

    if (shut := shut_gio()) is not None:
        prefix, said = shut
        ungio = run(data, "--yes", "trash", "--", str(first), str(blocker), str(last),
                    wrap=prefix)
        assert ungio.returncode == 1, ungio.stderr
        assert said in ungio.stderr, ungio.stderr
        # Once, and before the list is walked: "settled once, before anything
        # is trashed".
        assert ungio.stderr.count(said) == 1, ungio.stderr
        assert first.exists() and last.exists() and blocker.exists()
    assert str(first.resolve()) in run(data, "export").stdout

    # With the blocker out of the list, the whole set goes.
    swept = run(data, "--yes", "rm", "--", str(first), str(last))
    assert swept.returncode == 0, swept.stderr
    assert not first.exists() and not last.exists()

    # And it is written down, in the same voice mv and cp use for their own
    # divergence. Flattened, because prose wraps.
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

        # A link pointing nowhere. It is a path that is there, so it moves.
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
        # A real folder is still refused: that divergence is deliberate.
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
        # not a divergence -- so it is measured here and written down there.
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

    # A recorder in place of every opener, so nothing on this desktop is ever
    # started -- `open` refuses before it launches, and this pins that too.
    log = room / "opened.log"
    recording = recording_openers(log)
    if recording is not None:
        unopened = run(data, "open", str(keeper), str(first), str(second), wrap=recording)
        assert unopened.returncode == 2, unopened.stderr
        for absent in (first, second):
            assert f"path missing: {absent}" in unopened.stderr, unopened.stderr
        assert not log.exists(), "open launched the head of a list it was going to refuse"

    # A mixed list names each operand for its own reason.
    folder = room / "dirA"
    folder.mkdir()
    mixed = run(data, "--yes", "rm", "--", str(folder), str(first))
    assert mixed.returncode == 2, mixed.stderr
    assert f"path missing: {first}" in mixed.stderr, mixed.stderr
    assert f"{folder} is a directory" in mixed.stderr, mixed.stderr
    assert folder.is_dir()

    # A missing opener is the exception and is said once: it is the same answer
    # for every path in the list. Same rule as `trash`'s missing gio.
    if (hidden := hidden_openers()) is not None:
        unopenable = run(data, "open", str(keeper), str(keeper), wrap=hidden)
        assert unopenable.returncode == 1, unopenable.stderr
        assert unopenable.stderr.count("no xdg-open") == 1, unopenable.stderr

def check_reindex_names_the_path_it_walked_away_from(data, workspace):
    """--quiet is "No stderr except errors", and every exit 1 here is an error.

    Each of reindex's ways of walking away from a path names the path under
    --quiet, so an exit 1 never comes with nothing on either stream."""
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
    """verify names `tag clear` as the repair for xattr-invalid, and it exits
    nonzero when it has repaired nothing.

    A path with no index row has no record of the drop, so exit 0 there would
    assert a removal that did not happen. A path that
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
        # Still the same problem, and the repair it names says which case it
        # cannot settle -- the way xattr-differs does.
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
    # is clean.
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
    """__complete does not offer the flags `ls` would refuse.

    --tag, --any and --since are mutually exclusive with --untagged, so neither
    side is offered once the other is on the line. clap_complete derives the offer from
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

    A write that changes no tag -- `tag clear` or `tag rm` on a path with no row
    -- carries the stamp the row had, which for a path with no row is none."""
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
    """Five verbs say `Permission denied`, not `path missing`, about a file
    behind a shut directory.

    Every operand of `rm`, `trash`, `mv`, `cp` and `open` answers as `ls` does
    and as /usr/bin/rm does, exit 1. 2 is a path that is not there; a directory
    somebody chmod'd shut is the 1 the table gives an answer the run could not
    finish."""
    room = workspace / "shut-operands"
    shut = room / "shut"
    shut.mkdir(parents=True)
    subject = shut / "s.txt"
    subject.write_text("s")
    spare = room / "ok.txt"
    spare.write_text("o")
    assert run(data, "tag", "add", "--tag", "shutop", "--", str(subject)).returncode == 0
    resolved = str(subject.resolve())

    # A recorder in place of every opener, so nothing on this machine's
    # desktop is ever started; `open` is left out where there is none.
    log = room / "opened.log"
    only_fake = recording_openers(log)

    landing = room / "landed.txt"
    verbs = (("--yes", "rm"), ("--yes", "trash"), ("mv",), ("cp",)) \
        + ((("open",),) if only_fake is not None else ())

    def operands(verb):
        return [str(subject)] + ([str(landing)] if verb in ("mv", "cp") else [])

    shut.chmod(0o000)
    try:
        for globals_ in verbs:
            verb = globals_[-1]
            args = (*globals_, *operands(verb))
            refused = run(data, *args, wrap=only_fake if verb == "open" else ())
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
            absent = run(data, *globals_, *gone, wrap=only_fake if verb == "open" else ())
            assert absent.returncode == 2, (verb, absent.returncode, absent.stderr)
            assert "path missing" in absent.stderr, (verb, absent.stderr)

        # One of each names both and reports the missing path, which is the
        # settled answer of the two.
        mixed = run(data, "--yes", "rm", str(room / "not-here.txt"), str(subject))
        assert mixed.returncode == 2, mixed.stderr
        assert "path missing" in mixed.stderr and "Permission denied" in mixed.stderr, \
            mixed.stderr

        # The destination side answers the same way: the
        # source is there, the directory it is asked to land in is not readable.
        blocked = run(data, "mv", str(spare), str(shut / "new.txt"))
        assert blocked.returncode == 1, blocked.stderr
        assert "Permission denied" in blocked.stderr, blocked.stderr
        assert spare.exists()

        # The read verbs: `ls` skips the row and says so, `prune`
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
    """batch carries the status of a line that ran to the end and failed.

    A line that aborts is propagated -- a missing path gives exit 2 and a full
    rollback. One that completes with a nonzero status -- `reindex` over a file
    whose xattr the filesystem refuses, `tag clear` on a path with no row whose
    removal was refused -- has landed its rows, so the commit stands and the
    batch carries the 1 the line would have exited with."""
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
    """`export | import -` works in a shell that exports OMAFILES_JSON.

    `--json` is a global with an environment default behind it, so both ends of
    the pipe take it and `export` writes one array. jsonl stays the interchange
    form and what `export` writes when nothing asks otherwise; a shape `export`
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
    expanded, and `#` is a comment only in the first column: `tag add --tag t --
    c.txt # a note` hands three more operands to the verb."""
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
    """Two lines about one chmod agree with each other.

    A directory's detail says what the walk did, so it stays true beside a row
    from beneath that same directory that was checked and reported. The text
    form cannot spell a path holding a tab or a newline, and only --json can."""
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
    """A dry run does not take the write lock it never uses.

    It commits nothing, so with the index directory read-only it loads the
    index the way `ls`, `export` and `verify` do. And it prints what the real
    run prints: the destination, for `mv` and `cp`."""
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

    # The preview and the result are one shape, in text as under --json: both
    # name where the file went.
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
    """Two places the program describes itself.

    `mv a b c` is told the verb takes exactly two paths, not that they are
    required. And `completions --help`, the one verb that refuses --json, does
    not print the generic `Globals: --json ...` footer."""
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
    """A filename that is not valid UTF-8 is reported, not dropped in silence.

    A path here is text: the index is UTF-8 by definition and so is an xattr
    value, so such a name can be acted on by nothing in this program. `ls
    --untagged` names the entry it cannot spell rather than calling the
    directory clean -- including a spellable file under an unspellable parent."""
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

    # `verify` and `reindex` walk the same entries, and say it too. `verify`
    # says it in the shape it answers in, because a problem only stderr
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

    # `prune` walks the index and not the disk: a row can only hold a name that
    # was spellable when it was written.
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
    """`reindex`, `prune`, `batch` and `import` answer in a declared JSON shape.

    The row is the index row; a mirror the filesystem refused is the run's
    refusal, and the run says it with exit 1 and a line on stderr."""
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
        # The copy the filesystem refused changed nothing, so no row is a receipt.
        assert json.loads(refused.stdout) == [], refused.stdout

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
    name and writing it again could only fail on a file nobody may write. Only
    the crossing copy leaves the mirror behind, and that one has to land it."""
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
    """batch --json names only rows the commit kept.

    What the run wrote is what the index holds when it ends, once per path:
    `tag add` then `tag clear` on one path prints nothing for it, and three `tag
    add` lines on one path print one row. The dry run names each path once
    too."""
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
    """import --json prints a row per path rather than per dump line: two lines
    naming one path are one row in the index and one in the array."""
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
    """A tab or newline in an xattr value stays inside the detail field.

    The detail quotes the bytes a file carries and those are anybody's, so it
    is escaped -- on `xattr-invalid` above all, the kind that exists to report a
    value nothing can be read out of. The path field cannot be escaped, because
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

    # A value that is not valid UTF-8 is spelled `\xHH` the way a name that is
    # not is, never as U+FFFD, in both forms -- and both places a reader looks
    # say so.
    os.setxattr(subject, "user.omafiles.tags", b"ii\xffjj")
    mangled = run(data, "verify", str(room))
    assert "xattr=ii\\xffjj;" in mangled.stdout and "\ufffd" not in mangled.stdout, \
        mangled.stdout
    shaped = json.loads(run(data, "--json", "verify", str(room)).stdout)
    assert "xattr=ii\\xffjj;" in shaped["problems"][0]["detail"], shaped
    os.removexattr(subject, "user.omafiles.tags")

    reference = " ".join((ROOT / "docs/cli.md").read_text().split())
    assert "a tab there is written `\\t`, a newline `\\n`" in reference
    assert "and a NUL, is written `\\xHH` and never as U+FFFD" in reference


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


def check_an_unplugged_root_is_not_a_deleted_file(data, workspace):
    """The whole reason a root can be declared, end to end.

    Unmount, prune, remount somewhere else, tags still in place. Without a
    declared root an unmounted tree is ENOENT from a stat, which is exactly what
    every file in it having been deleted looks like -- and on a filesystem with
    no `user.*` namespace the rows are the only copy of those tags.

    Nothing is really mounted here. `--check` is a name that exists only when
    the storage does, which is the surer of the two tests anyway: an unmounted
    mount point is usually still an empty directory."""
    home = workspace / "rootcase"
    config = home / "config"
    here = home / "mnt-a"
    elsewhere = home / "mnt-b"
    for path in (config, here, elsewhere):
        path.mkdir(parents=True)
    env = {"XDG_CONFIG_HOME": str(config)}

    (here / "docs").mkdir()
    contract = here / "docs" / "contract.pdf"
    contract.write_text("c")
    marker = here / ".plugged-in"
    marker.write_text("")

    declared = run(data, "roots", "add", "--id", "archive", "--check", ".plugged-in",
                   "--", str(here), extra_env=env)
    assert declared.returncode == 0, declared.stderr
    tagged = run(data, "tag", "add", "--tag", "contract", "--", str(contract), extra_env=env)
    assert tagged.returncode == 0, tagged.stderr

    # The row is written as the root and the rest of the way there, which is
    # what makes the next part possible at all.
    rows = [json.loads(line) for line in run(data, "export", extra_env=env).stdout.splitlines()]
    stored = [row for row in rows if row["path"].endswith("contract.pdf")]
    assert stored, rows
    raw = (data / "omafiles" / "index.jsonl").read_text().splitlines()
    written = [json.loads(line) for line in raw if "contract.pdf" in line]
    assert written[0]["root"] == "archive", written
    assert written[0]["path"] == "docs/contract.pdf", written

    listed = run(data, "roots", extra_env=env)
    assert listed.stdout.splitlines() == [f"archive\t{here.resolve()}\there"], listed.stdout

    # Unplugged: the storage goes, the mount point stays, exactly as it would.
    (here / "docs").rename(home / "docs-away")
    marker.rename(home / "marker-away")
    away = run(data, "roots", extra_env=env)
    assert away.stdout.splitlines() == [f"archive\t{here.resolve()}\taway"], away.stdout

    # `prune --all` with the storage away keeps the rows.
    pruned = run(data, "--yes", "prune", "--all", extra_env=env)
    assert pruned.returncode == 0, pruned.stderr
    assert str(contract.resolve()) not in pruned.stdout, pruned.stdout
    assert "root archive is declared and not here" in pruned.stderr, pruned.stderr
    assert "contract.pdf" in run(data, "export", extra_env=env).stdout

    # And verify says it could not check the row rather than calling it a
    # problem it found. Handed the away root itself, it refuses the operand, as
    # every verb does.
    checked = run(data, "verify", str(here.parent), extra_env=env)
    assert "missing" not in checked.stdout, checked.stdout
    assert "was not checked" in checked.stderr, checked.stderr
    operand = run(data, "verify", str(here), extra_env=env)
    assert operand.returncode == 1 and "declared and not here" in operand.stderr, \
        (operand.returncode, operand.stderr)

    # Remounted somewhere else. The declaration moves; the rows do not.
    (home / "docs-away").rename(elsewhere / "docs")
    (home / "marker-away").rename(elsewhere / ".plugged-in")
    roots_file = config / "omafiles" / "roots.jsonl"
    moved = [json.loads(line) for line in roots_file.read_text().splitlines() if line.strip()]
    for root in moved:
        if root["id"] == "archive":
            root["path"] = str(elsewhere.resolve())
    roots_file.write_text("".join(json.dumps(root) + "\n" for root in moved))

    back = run(data, "ls", "--tag", "contract", extra_env=env)
    assert back.returncode == 0, back.stderr
    assert back.stdout.splitlines() == [str((elsewhere / "docs" / "contract.pdf").resolve())], \
        back.stdout

    # `migrate` is never required -- a row takes its new form the next time
    # anything writes it -- but it says how many the declaration took in.
    migrated = run(data, "roots", "migrate", extra_env=env)
    assert migrated.returncode == 0, migrated.stderr
    assert "1 under a root" in migrated.stdout, migrated.stdout

    # Undeclaring carries its rows with it, as the absolute paths they resolve
    # to now: a row names a declaration, so leaving them behind would leave rows
    # nothing can place and every verb refusing to read the index.
    gone = run(data, "roots", "rm", "--id", "archive", extra_env=env)
    assert gone.returncode == 0, gone.stderr
    absolute = [json.loads(line) for line in
                (data / "omafiles" / "index.jsonl").read_text().splitlines()
                if "contract.pdf" in line]
    assert "root" not in absolute[0], absolute
    assert absolute[0]["path"] == str((elsewhere / "docs" / "contract.pdf").resolve()), absolute

    # And with no root declared, the old behaviour is exactly the old behaviour.
    (elsewhere / "docs" / "contract.pdf").unlink()
    swept = run(data, "--yes", "prune", "--all", extra_env=env)
    assert swept.returncode == 0, swept.stderr
    assert "contract.pdf" in swept.stdout, swept.stdout


def check_a_sweep_only_ever_adds(data, workspace):
    """The routine as a verb: it adopts, it merges both ways, it drops nothing.

    The tags travel with the file in its own xattr, so something else moving,
    copying or restoring a file loses the index row and not the tags. Nothing
    read them back without somebody deciding which side won: plain `reindex`
    writes the row over the file and `reindex --from-xattr` writes the file over
    the row, and a run nobody is watching cannot be trusted with either. This
    one takes the union, both ways, and never removes -- which is what makes it
    safe to leave running."""
    home = workspace / "sweepcase"
    config = home / "config"
    state = home / "state"
    cache = home / "cache"
    tree = home / "tree"
    for path in (config, state, cache, tree):
        path.mkdir(parents=True)
    # Its own state and cache directories as well as its own config: the receipt
    # goes under $XDG_STATE_HOME and the list of names under $XDG_CACHE_HOME,
    # and a suite that wrote into the real ones would be leaving files on the
    # machine that ran it.
    env = {"XDG_CONFIG_HOME": str(config), "XDG_STATE_HOME": str(state),
           "XDG_CACHE_HOME": str(cache)}

    # Never having run is an answer, not an error.
    fresh = run(data, "sweep", "--status", extra_env=env)
    assert fresh.returncode == 0, fresh.stderr
    assert fresh.stdout.splitlines() == ["never"], fresh.stdout

    # A file something else put here, carrying the tags it left with, that the
    # index has never heard of -- which is what a restore from a backup, or a
    # `cp --preserve=xattr`, looks like from in here.
    restored = tree / "restored.md"
    restored.write_text("r")
    os.setxattr(restored, "user.omafiles.tags", b"trip,photo")
    swept = run(data, "sweep", str(tree), extra_env=env)
    assert swept.returncode == 0, swept.stderr
    assert swept.stdout.splitlines() == [str(restored.resolve())], swept.stdout
    assert "adopted 1" in swept.stderr, swept.stderr
    assert run(data, "tag", "ls", "--", str(restored),
               extra_env=env).stdout.split() == ["photo", "trip"]

    # Both ways, and never a removal. The row holds a tag the file does not and
    # the file carries one the row does not; either `reindex` would settle this
    # by throwing one of them away.
    kept = tree / "kept.md"
    kept.write_text("k")
    run(data, "tag", "add", "--tag", "row-side", "--", str(kept), extra_env=env)
    os.setxattr(kept, "user.omafiles.tags", b"file-side")
    merged = run(data, "sweep", str(tree), extra_env=env)
    assert merged.returncode == 0, merged.stderr
    assert "merged 1" in merged.stderr, merged.stderr
    assert run(data, "tag", "ls", "--", str(kept),
               extra_env=env).stdout.split() == ["file-side", "row-side"]
    assert os.getxattr(kept, "user.omafiles.tags") == b"file-side,row-side"
    # With both copies holding the same thing there is nothing left to report
    # but the adopted value's order, a spelling `sweep` leaves to `reindex`.
    assert run(data, "verify", str(tree), extra_env=env).stdout == \
        f"xattr-differs\t{restored.resolve()}\tindex=photo,trip xattr=trip,photo\n"

    # A value no tag can be read out of is the one thing it will not rewrite:
    # salvaging the readable half back over it would delete the rest, and a verb
    # that may not remove a tag may not remove one that way either.
    odd = tree / "odd.md"
    odd.write_text("o")
    os.setxattr(odd, "user.omafiles.tags", b"good,Bad Tag")
    assert run(data, "sweep", str(tree), extra_env=env).returncode == 0
    assert run(data, "tag", "ls", "--", str(odd), extra_env=env).stdout.split() == ["good"]
    assert os.getxattr(odd, "user.omafiles.tags") == b"good,Bad Tag"

    # A row whose file is gone is counted, named as somebody else's job, and
    # left exactly where it is. Dropping one is the single thing an unwatched
    # run must never do: a row is sometimes the only copy of those tags here.
    restored.unlink()
    counted = run(data, "sweep", str(tree), extra_env=env)
    assert counted.returncode == 0, counted.stderr
    assert "gone 1" in counted.stderr, counted.stderr
    assert "omafiles prune" in counted.stderr, counted.stderr
    assert "restored.md" in run(data, "export", extra_env=env).stdout

    # A writer that already holds the index does not get waited out, and the run
    # that finds it held does not fail -- it steps aside, and the next one does
    # the same work over the same trees.
    lock = forge_lock(data)
    try:
        aside = run(data, "sweep", str(tree), extra_env=env)
    finally:
        lock.unlink(missing_ok=True)
    assert aside.returncode == 0, (aside.returncode, aside.stderr)
    assert "stepped aside" in aside.stderr, aside.stderr
    assert str(tree.resolve()) in aside.stderr, aside.stderr

    # With no directory the scope is the declared roots and not wherever the run
    # was started from. A machine that has declared none has no such list, so it
    # is asked for one rather than handed the current directory.
    nowhere = run(data, "sweep", extra_env=env, cwd=tree)
    assert nowhere.returncode == 1, (nowhere.returncode, nowhere.stderr)
    assert "no root is declared" in nowhere.stderr, nowhere.stderr
    declared = run(data, "roots", "add", "--id", "swept", "--", str(tree), extra_env=env)
    assert declared.returncode == 0, declared.stderr
    rooted = run(data, "sweep", extra_env=env)
    assert rooted.returncode == 0, rooted.stderr

    # And the receipt says what that run found, in the shape `tags` and
    # `roots ls` print in: one fact to a line, tab separated.
    receipt = dict(line.split("\t", 1) for line
                   in run(data, "sweep", "--status", extra_env=env).stdout.splitlines())
    assert receipt["tree"] == str(tree.resolve()), receipt
    assert receipt["gone"] == "1", receipt
    assert (state / "omafiles" / "sweep.json").is_file()

    # A dry run sweeps nothing, and a receipt is for what happened.
    before = (state / "omafiles" / "sweep.json").read_text()
    previewed = tree / "previewed.md"
    previewed.write_text("p")
    os.setxattr(previewed, "user.omafiles.tags", b"previewed")
    preview = run(data, "--dry-run", "sweep", str(tree), extra_env=env)
    assert preview.returncode == 0, preview.stderr
    assert str(previewed.resolve()) in preview.stdout, preview.stdout
    assert run(data, "tag", "ls", "--", str(previewed), extra_env=env).stdout.strip() == ""
    assert (state / "omafiles" / "sweep.json").read_text() == before

    # The JSON shape docs/cli.md declares.
    shaped = json.loads(run(data, "--json", "sweep", str(tree), extra_env=env).stdout)
    assert set(shaped) == {"trees", "adopted", "merged", "gone", "unreadable",
                           "skipped", "listed", "unlistable"}, shaped
    assert shaped["adopted"] == [str(previewed.resolve())], shaped
    assert shaped["gone"] == [str(restored.resolve())], shaped
    assert shaped["trees"] == [str(tree.resolve())], shaped

    # Undeclaring carries the rows out as absolute paths, so what this case
    # declared cannot leave the index unreadable to everything after it.
    assert run(data, "roots", "rm", "--id", "swept", extra_env=env).returncode == 0

def check_a_list_of_names_is_not_the_index(data, workspace):
    """The cache `--untagged` answers out of, and the line around it.

    `--untagged` is the one listing here that cannot come out of the index: the
    index holds rows that carry a tag, and what has no tag has no row. So the
    exact answer costs a walk of the tree, every time, for a question somebody
    wants to ask from a panel. The list of names is what makes it instant, and
    the whole risk of having one is that it gets mistaken for the index -- so
    what is measured here is that it is derived, dated, disposable, and that
    the exact answer is still there beside it."""
    home = workspace / "namescase"
    config = home / "config"
    state = home / "state"
    cache = home / "cache"
    tree = home / "tree"
    for path in (config, state, cache, tree):
        path.mkdir(parents=True)
    env = {"XDG_CONFIG_HOME": str(config), "XDG_STATE_HOME": str(state),
           "XDG_CACHE_HOME": str(cache)}

    (tree / "sub").mkdir()
    filed = tree / "sub" / "filed.md"
    loose = tree / "sub" / "loose.md"
    for path in (filed, loose):
        path.write_text("x")

    # An id is a name and not a path: it is what every index row carries and it
    # is the file name of the list, so `..` in one would be a list written
    # somewhere else entirely. Folded as it is declared, the way a tag is.
    refused = run(data, "roots", "add", "--id", "../oops", "--", str(tree), extra_env=env)
    assert refused.returncode == 1, refused.stderr
    assert "is not an id" in refused.stderr, refused.stderr
    assert run(data, "roots", "add", "--id", "Names", "--",
               str(tree), extra_env=env).returncode == 0
    assert run(data, "roots", extra_env=env).stdout.split("\t")[0] == "names"

    run(data, "tag", "add", "--tag", "filed", "--", str(filed), extra_env=env)

    # Nothing has swept, so there is no list -- and that is said rather than
    # answered around. A short answer is exit 1 here as it is everywhere else.
    blind = run(data, "ls", "--untagged", "--cached", extra_env=env)
    assert blind.returncode == 1, (blind.returncode, blind.stdout)
    assert blind.stdout == "", blind.stdout
    assert "has no list of names yet" in blind.stderr, blind.stderr

    assert run(data, "sweep", extra_env=env).returncode == 0
    listing = cache / "omafiles" / "names" / "names"
    assert listing.is_file(), sorted(cache.rglob("*"))
    # Relative to the root, sorted, one to a line: a remount moves the root and
    # invalidates nothing, and `sort` is what every reader is allowed to assume.
    assert listing.read_text().splitlines() == ["sub", "sub/filed.md", "sub/loose.md"]

    # And the cached answer is the walking answer, to the path.
    walked = run(data, "ls", "--untagged", str(tree), extra_env=env)
    cached = run(data, "ls", "--untagged", "--cached", extra_env=env)
    assert cached.returncode == 0, cached.stderr
    assert cached.stdout == walked.stdout, (cached.stdout, walked.stdout)
    assert str(filed.resolve()) not in cached.stdout, cached.stdout
    assert "from a list of names built" in cached.stderr, cached.stderr

    # It is as old as the last sweep and says so, which is the whole contract:
    # a file made since is not in it, and nothing here stats a path on the way
    # out, because that would be the walk again.
    later = tree / "sub" / "later.md"
    later.write_text("l")
    loose.unlink()
    stale = run(data, "ls", "--untagged", "--cached", extra_env=env)
    assert str(later.resolve()) not in stale.stdout, stale.stdout
    assert str(loose.resolve()) in stale.stdout, stale.stdout
    # The exact answer is still there, beside it and not replaced by it.
    fresh = run(data, "ls", "--untagged", str(tree), extra_env=env)
    assert str(later.resolve()) in fresh.stdout, fresh.stdout
    assert str(loose.resolve()) not in fresh.stdout, fresh.stdout

    # The age is said in the spelling `--since` takes, so one vocabulary covers
    # asking about a span and being told one.
    os.utime(listing, (time.time() - 10800, time.time() - 10800))
    dated = run(data, "ls", "--untagged", "--cached", extra_env=env)
    assert "built 3h ago" in dated.stderr, dated.stderr
    assert run(data, "--quiet", "ls", "--untagged", "--cached",
               extra_env=env).stderr == ""

    # Scoped, it is scoped the way the walk is: `[dir]` is about what is in the
    # directory, not about the directory.
    scoped = run(data, "ls", "--untagged", "--cached", str(tree / "sub"), extra_env=env)
    # A line and not a substring: every path under it starts with the directory.
    assert str((tree / "sub").resolve()) not in scoped.stdout.splitlines(), scoped.stdout
    assert str(loose.resolve()) in scoped.stdout.splitlines(), scoped.stdout

    # Two refusals, each naming the way to ask the question instead.
    everywhere = run(data, "ls", "--untagged", "--cached", str(workspace), extra_env=env)
    assert everywhere.returncode == 1, everywhere.stderr
    assert "under no declared root" in everywhere.stderr, everywhere.stderr
    alone = run(data, "ls", "--cached", extra_env=env)
    assert alone.returncode == 1, alone.stderr
    assert "answers --untagged" in alone.stderr, alone.stderr

    # Disposable, and nothing refuses to run without it. Deleting the whole
    # directory costs one sweep and no information.
    shutil.rmtree(cache / "omafiles")
    assert run(data, "ls", "--untagged", str(tree), extra_env=env).returncode == 0
    assert run(data, "verify", str(tree), extra_env=env).returncode == 0
    assert run(data, "sweep", extra_env=env).returncode == 0
    assert listing.is_file()

    # A list belongs to a declaration: undeclaring the root takes it away, since
    # nothing will ever read a list for a root nobody declares.
    assert run(data, "roots", "rm", "--id", "Names", extra_env=env).returncode == 0
    assert not listing.exists(), sorted(cache.rglob("*"))

def check_mv_renames_a_set_by_rule(data, workspace):
    """mv --replace/--with and --pattern: a set renamed in place, tags kept.

    Every destination is settled before any rename is made, the plan a dry run
    prints is the receipt the run prints, and the rows follow the files."""
    room = workspace / "byrule"
    room.mkdir()
    shots = [room / f"img-{n}.jpg" for n in ("a", "b", "c")]
    for shot in shots:
        shot.write_text(shot.name)
    run(data, "tag", "add", "--tag", "trip", "--", str(shots[0]), str(shots[2]))
    beside = room / "notes.txt"
    beside.write_text("n")

    # The flags go together, the template only knows four tokens, and a bare
    # --pattern is a template with nothing in it.
    lonely = run(data, "mv", "--with", "x", "--", str(shots[0]))
    assert lonely.returncode == 1 and "go together" in lonely.stderr, lonely.stderr
    assert run(data, "mv", "--replace", "", "--with", "x", "--", str(shots[0])).returncode == 1
    bogus = run(data, "mv", "--pattern", "{date}{ext}", "--", str(shots[0]))
    assert bogus.returncode == 1 and "unknown token {date}" in bogus.stderr, bogus.stderr
    assert run(data, "mv", "--pattern", "", "--", str(shots[0])).returncode == 1
    assert run(data, "mv", "--replace", "img", "--with", "pic").returncode == 1
    assert all(shot.exists() for shot in shots), "a refusal renamed something"

    # The plan, before anything moves: old, tab, new, one to a line, and under
    # --json the object `mv <old> <new>` answers in, one per rename.
    planned = run(data, "--dry-run", "mv", "--replace", "img", "--with", "pic",
                  "--", *map(str, shots))
    assert planned.returncode == 0, planned.stderr
    assert planned.stdout.splitlines() == [
        f"{shot.resolve()}\t{room.resolve() / shot.name.replace('img', 'pic')}"
        for shot in shots], planned.stdout
    assert all(shot.exists() for shot in shots), "a dry run renamed something"
    shaped = json.loads(run(data, "--json", "--dry-run", "mv", "--replace", "img",
                            "--with", "pic", "--", *map(str, shots)).stdout)
    assert [set(one) for one in shaped] == [{"from", "to", "rows"}] * 3, shaped
    assert [[row["tags"] for row in one["rows"]] for one in shaped] == [[["trip"]], [], [["trip"]]]

    # The run prints the same receipt, and the rows went with the files.
    renamed = run(data, "mv", "--replace", "img", "--with", "pic", "--", *map(str, shots))
    assert renamed.returncode == 0, renamed.stderr
    assert renamed.stdout == planned.stdout, renamed.stdout
    pics = [room / f"pic-{n}.jpg" for n in ("a", "b", "c")]
    assert all(pic.exists() for pic in pics) and not any(shot.exists() for shot in shots)
    assert run(data, "tag", "ls", "--", str(pics[0])).stdout.split() == ["trip"]
    assert run(data, "tag", "ls", "--", str(pics[2])).stdout.split() == ["trip"]
    assert run(data, "verify", str(room)).returncode == 0

    # A template numbers the set in operand order, padded to the count, and
    # the extension keeps its dot. A name the rule leaves alone is left alone.
    numbered = run(data, "mv", "--pattern", "{stem}-{n}{ext}", "--",
                   str(pics[2]), str(pics[0]))
    assert numbered.returncode == 0, numbered.stderr
    assert (room / "pic-c-1.jpg").exists() and (room / "pic-a-2.jpg").exists()
    same = run(data, "mv", "--pattern", "{name}", "--", str(pics[1]), str(beside))
    assert same.returncode == 0 and same.stdout == "", same.stdout
    assert pics[1].exists() and beside.exists()
    padded = run(data, "--dry-run", "mv", "--pattern", "{n}{ext}", "--",
                 *[str(room / f"n{i}.x") for i in range(10)])
    assert padded.returncode == 2, padded.stderr        # they are not there
    tens = [room / f"t{i}.x" for i in range(10)]
    for one in tens:
        one.write_text("t")
    padded = run(data, "--dry-run", "mv", "--pattern", "{n}{ext}", "--", *map(str, tens))
    assert padded.stdout.splitlines()[0].endswith("\t" + str(room.resolve() / "01.x")), padded.stdout
    assert padded.stdout.splitlines()[-1].endswith("\t" + str(room.resolve() / "10.x")), padded.stdout
    # A hidden file has no extension, and `--with ''` deletes the text.
    hidden = room / ".secret-notes"
    hidden.write_text("h")
    dropped = run(data, "--dry-run", "mv", "--replace", "-notes", "--with", "",
                  "--pattern", "{stem}{ext}", "--", str(hidden))
    assert dropped.stdout.strip().endswith("\t" + str(room.resolve() / ".secret")), dropped.stdout

    # A name that is taken refuses the whole set before anything moves, and
    # so do two operands landing on one name -- including a name an operand
    # about to move still holds.
    taken = run(data, "mv", "--replace", "pic-b", "--with", "notes", "--pattern",
                "{stem}.txt", "--", str(pics[1]), str(room / "pic-c-1.jpg"))
    assert taken.returncode == 1 and "already exists" in taken.stderr, taken.stderr
    assert pics[1].exists() and (room / "pic-c-1.jpg").exists()
    clash = run(data, "mv", "--pattern", "same{ext}", "--", str(pics[1]), str(room / "pic-c-1.jpg"))
    assert clash.returncode == 1 and "would both become" in clash.stderr, clash.stderr
    shift = run(data, "mv", "--replace", "t0", "--with", "t1", "--", str(tens[0]), str(tens[1]))
    assert shift.returncode == 1 and "already exists" in shift.stderr, shift.stderr

    # And the plain form still takes exactly two.
    assert "takes exactly two paths" in run(data, "mv", str(pics[1])).stderr
    assert run(data, "verify", str(room)).returncode == 0


def check_ls_name_asks_the_disk_or_the_list(data, workspace):
    """--name: a shell pattern over names, on the disk, the list, or the rows.

    Alone it walks like --untagged does, --cached answers it out of the list
    sweep built, and beside a tag it narrows the rows the tag chose."""
    home = workspace / "byname"
    config = home / "config"
    cache = home / "cache"
    tree = home / "tree"
    for path in (config, cache, tree / "sub"):
        path.mkdir(parents=True)
    env = {"XDG_CONFIG_HOME": str(config), "XDG_STATE_HOME": str(home / "state"),
           "XDG_CACHE_HOME": str(cache)}
    papers = [tree / "a.pdf", tree / "b.PDF", tree / "sub" / "c.pdf"]
    other = tree / "d.txt"
    for path in papers + [other]:
        path.write_text("x")
    run(data, "tag", "add", "--tag", "paper", "--", str(papers[0]), extra_env=env)

    # The walk: every name that fits, folded, tags and all, sorted by path.
    walked = run(data, "ls", "--name", "*.pdf", str(tree), extra_env=env)
    assert walked.returncode == 0, walked.stderr
    assert walked.stdout.splitlines() == sorted(str(p.resolve()) for p in papers), walked.stdout
    rows = json.loads(run(data, "--json", "ls", "--name", "*.pdf", str(tree),
                          extra_env=env).stdout)
    assert {row["path"]: row["tags"] for row in rows}[str(papers[0].resolve())] == ["paper"], rows
    assert run(data, "ls", "--name", "", str(tree), extra_env=env).returncode == 1
    assert run(data, "ls", "--name", "invoice-??.*", str(tree), extra_env=env).stdout == ""

    # Beside a tag it narrows the rows and walks nothing.
    narrowed = run(data, "ls", "--tag", "paper", "--name", "*.pdf", extra_env=env)
    assert narrowed.stdout.splitlines() == [str(papers[0].resolve())], narrowed.stdout
    assert run(data, "ls", "--tag", "paper", "--name", "*.txt", extra_env=env).stdout == ""

    # --cached answers it out of the list, with the list's own note and
    # refusals, and refuses beside a tag the way it refuses alone.
    assert run(data, "roots", "add", "--id", "byname", "--", str(tree),
               extra_env=env).returncode == 0
    assert run(data, "sweep", extra_env=env).returncode == 0
    listed = run(data, "ls", "--name", "*.pdf", "--cached", extra_env=env)
    assert listed.returncode == 0, listed.stderr
    assert listed.stdout == walked.stdout, (listed.stdout, walked.stdout)
    assert "from a list of names built" in listed.stderr, listed.stderr
    refused = run(data, "ls", "--tag", "paper", "--name", "*.pdf", "--cached", extra_env=env)
    assert refused.returncode == 1 and "answers --untagged and --name" in refused.stderr

    # And the walk says what it could not walk, in --untagged's words.
    shut = tree / "shut"
    shut.mkdir()
    shut.chmod(0o000)
    try:
        short = run(data, "ls", "--name", "*.pdf", str(tree), extra_env=env)
        assert short.returncode == 1 and "unreadable" in short.stderr, short.stderr
    finally:
        shut.chmod(0o755)
    assert "--name" in run(data, "__complete", "--", "ls", "-").stdout.split()
    # The root goes with the case, and its rows come back as absolute paths:
    # a row naming a root nobody declares is one no later case can read.
    assert run(data, "roots", "rm", "--id", "byname", extra_env=env).returncode == 0


def check_recent_reads_the_desktops_list(data, workspace):
    """recent --desktop: what the desktop opened lately, from its own file.

    Newest first, local files that are still there, each with the row it has;
    a list that is not there is an empty answer and not an error."""
    room = workspace / "opened"
    room.mkdir()
    older = room / "older.md"
    newer = room / "newer.pdf"
    gone = room / "gone.txt"
    for path in (older, newer):
        path.write_text("x")
    run(data, "tag", "add", "--tag", "opened", "--", str(newer))

    empty = run(data, "recent", "--desktop")
    assert empty.returncode == 0 and empty.stdout == "", (empty.returncode, empty.stderr)

    xbel = data / "recently-used.xbel"
    xbel.write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<xbel version="1.0"
      xmlns:bookmark="http://www.freedesktop.org/standards/desktop-bookmarks"
      xmlns:mime="http://www.freedesktop.org/standards/shared-mime-info">
  <bookmark href="file://{older}" added="2026-09-01T10:00:00.500000Z" modified="2026-09-01T10:00:00Z" visited="2026-09-01T10:00:00Z">
    <info><metadata owner="http://freedesktop.org"><mime:mime-type type="text/markdown"/></metadata></info>
  </bookmark>
  <bookmark href="file://{gone}" added="2026-09-03T10:00:00Z" modified="2026-09-03T10:00:00Z" visited="2026-09-03T10:00:00Z"/>
  <bookmark href="https://example.org/paper.pdf" added="2026-09-04T10:00:00Z" modified="2026-09-04T10:00:00Z" visited="2026-09-04T10:00:00Z"/>
  <bookmark href="file://{newer}" added="2026-09-02T10:00:00Z" modified="2026-09-02T10:00:00Z" visited="2026-09-02T12:30:00Z"/>
</xbel>
""")
    listed = run(data, "recent", "--desktop")
    assert listed.returncode == 0, listed.stderr
    assert listed.stdout.splitlines() == [str(newer.resolve()), str(older.resolve())], listed.stdout
    assert run(data, "recent", "--desktop", "1").stdout.splitlines() == [str(newer.resolve())]
    rows = json.loads(run(data, "--json", "recent", "--desktop").stdout)
    assert [(row["path"], row["tags"]) for row in rows] == \
        [(str(newer.resolve()), ["opened"]), (str(older.resolve()), [])], rows

    # A list that will not read is said, and the flag is the verb's alone.
    xbel.write_text("<xbel><bookmark href='file:///x'")
    broken = run(data, "recent", "--desktop")
    assert broken.returncode == 1 and "recently-used.xbel" in broken.stderr, broken.stderr
    xbel.unlink()
    assert "unexpected --desktop" in run(data, "ls", "--desktop").stderr
    assert "--desktop" in run(data, "__complete", "--", "recent", "-").stdout.split()


def mode_of(path):
    return os.lstat(path).st_mode & 0o7777


def check_chmod_sets_the_rwx_bits(data, workspace):
    """chmod: octal and coreutils' symbolic clauses, a dry run, and the receipt."""
    room = workspace / "modes"
    room.mkdir()
    subject = room / "m.txt"
    subject.write_text("m")
    subject.chmod(0o600)
    named = str(subject.resolve())

    octal = run(data, "chmod", "644", str(subject))
    assert octal.returncode == 0, octal.stderr
    assert mode_of(subject) == 0o644
    # Old, new, path: the path last, so a tab in a name cannot move the modes.
    assert octal.stdout == f"0600\t0644\t{named}\n", octal.stdout
    assert run(data, "chmod", "0755", str(subject)).returncode == 0
    assert mode_of(subject) == 0o755

    # Each clause on the mode the one before it left: 755, u-x 655, go-rx 600,
    # a+r 644. Then `=` sets a class outright, and one clause can hold more
    # than one operation.
    for mode, expected in (("u-x,go-rx,a+r", 0o644), ("g=w", 0o624), ("u=rwx-w", 0o524),
                           ("a=", 0o000), ("ugo+r,u+w", 0o644)):
        done = run(data, "chmod", mode, str(subject))
        assert done.returncode == 0, (mode, done.stderr)
        assert mode_of(subject) == expected, (mode, oct(mode_of(subject)))

    # A clause names who it is for: `+x` means something different under every
    # umask, and the rest are not modes at all.
    for mode in ("+x", "u", "9", "u+q", "", "u+r,", "g=u", "u+X", "12345"):
        refused = run(data, "chmod", mode, str(subject))
        assert refused.returncode == 1, (mode, refused.stdout)
        assert "invalid mode" in refused.stderr, (mode, refused.stderr)
        assert mode_of(subject) == 0o644, mode

    # A dry run prints what it would do and does none of it.
    dry = run(data, "--dry-run", "chmod", "600", str(subject))
    assert dry.returncode == 0, dry.stderr
    assert dry.stdout == f"0644\t0600\t{named}\n", dry.stdout
    assert mode_of(subject) == 0o644

    # The receipt under --json, one entry per path, in the order named.
    other = room / "n.txt"
    other.write_text("n")
    other.chmod(0o600)
    receipt = run(data, "--json", "chmod", "640", str(subject), str(other))
    assert receipt.returncode == 0, receipt.stderr
    assert json.loads(receipt.stdout) == {"modes": [
        {"path": named, "old": "0644", "new": "0640"},
        {"path": str(other.resolve()), "old": "0600", "new": "0640"}]}, receipt.stdout

    # A folder's own mode, and nothing under it.
    folder = room / "folder"
    folder.mkdir()
    inner = folder / "inner.txt"
    inner.write_text("i")
    inner.chmod(0o600)
    assert run(data, "chmod", "700", str(folder)).returncode == 0
    assert mode_of(folder) == 0o700 and mode_of(inner) == 0o600

    # The index and the tags are not its business.
    assert run(data, "tag", "add", "--tag", "moded", "--", str(subject)).returncode == 0
    index = data / "omafiles" / "index.jsonl"
    before = index.read_bytes()
    assert run(data, "chmod", "600", str(subject)).returncode == 0
    assert index.read_bytes() == before
    assert run(data, "tag", "ls", "--", str(subject)).stdout.split() == ["moded"]

    # A mode on disk cannot be put back by a rolled back index.
    rolled = run(data, "batch", "-", stdin=f"chmod 644 -- {subject}\n")
    assert rolled.returncode == 1 and "changes files on disk" in rolled.stderr, rolled.stderr
    assert mode_of(subject) == 0o600

    # A receipt it could not write is not a success.
    full = Path("/dev/full")
    if full.exists():
        env = cli_env(data)
        with full.open("w") as sink:
            lost = subprocess.run([str(CLI), "chmod", "644", str(subject)], cwd=ROOT, env=env,
                                  text=True, stdout=sink, stderr=subprocess.PIPE,
                                  stdin=subprocess.DEVNULL)
        assert lost.returncode == 1 and "chmod: write error" in lost.stderr, lost.stderr

    assert "chmod" in run(data, "__complete", "--", "").stdout.split()


def check_chmod_never_touches_the_special_bits(data, workspace):
    """setuid, setgid and sticky are never asked for and never taken away."""
    room = workspace / "special"
    room.mkdir()
    subject = room / "s.txt"
    subject.write_text("s")
    subject.chmod(0o644)

    for mode in ("4755", "2755", "1777", "u+s", "g+s", "o+t", "a+rst"):
        for dry in (("--dry-run",), ()):
            refused = run(data, *dry, "chmod", mode, str(subject))
            assert refused.returncode == 1, (mode, dry)
            assert "setuid, setgid or sticky" in refused.stderr, (mode, refused.stderr)
            assert refused.stdout == "", refused.stdout
            assert mode_of(subject) == 0o644, (mode, oct(mode_of(subject)))

    # What the file carries passes through, octal and symbolic alike. coreutils
    # `chmod 644` and `chmod u=rw` would both drop the setuid here.
    subject.chmod(0o4755)
    kept = run(data, "chmod", "644", str(subject))
    assert kept.returncode == 0, kept.stderr
    assert mode_of(subject) == 0o4644, oct(mode_of(subject))
    assert kept.stdout.startswith("4755\t4644\t"), kept.stdout
    assert run(data, "chmod", "u=rw,go-r", str(subject)).returncode == 0
    assert mode_of(subject) == 0o4600, oct(mode_of(subject))

    shared = room / "shared"
    shared.mkdir()
    shared.chmod(0o3777)
    assert run(data, "chmod", "go-w", str(shared)).returncode == 0
    assert mode_of(shared) == 0o3755, oct(mode_of(shared))
    shared.chmod(0o755)


def check_chmod_refuses_a_symlink_and_a_set_it_cannot_take(data, workspace):
    """A link is refused rather than followed, and a set is checked whole first."""
    room = workspace / "linked"
    room.mkdir()
    target = room / "target.txt"
    target.write_text("t")
    target.chmod(0o600)
    link = room / "link"
    link.symlink_to(target)
    dangling = room / "dangling"
    dangling.symlink_to(room / "nowhere")

    for dry in (("--dry-run",), ()):
        refused = run(data, *dry, "chmod", "644", str(link))
        assert refused.returncode == 1, refused.stdout
        assert f"{os.path.realpath(room)}/link is a symlink;" in refused.stderr, \
            refused.stderr
        assert mode_of(target) == 0o600
    # Refused as the link it is, not as a path that is not there.
    hanging = run(data, "chmod", "644", str(dangling))
    assert hanging.returncode == 1 and "is a symlink" in hanging.stderr, hanging.stderr

    # One operand it will not take refuses the set, and every one is named.
    plain = room / "plain.txt"
    plain.write_text("p")
    plain.chmod(0o600)
    mixed = run(data, "chmod", "644", str(plain), str(link), str(room / "nope"))
    assert mixed.returncode == 2, mixed.stderr
    assert "is a symlink" in mixed.stderr and "path missing" in mixed.stderr, mixed.stderr
    assert mixed.stdout == ""
    assert mode_of(plain) == 0o600, "part of a refused set was changed"

    missing = run(data, "chmod", "644", str(room / "nope"))
    assert missing.returncode == 2 and "path missing" in missing.stderr, missing.stderr
    assert run(data, "chmod", "644").returncode == 1

    # A path nobody may look at is not a path that is gone.
    if os.geteuid() == 0:
        return                               # root may look at everything
    shut = room / "shut"
    shut.mkdir()
    buried = shut / "buried.txt"
    buried.write_text("b")
    shut.chmod(0o000)
    try:
        blind = run(data, "chmod", "644", str(buried))
        assert blind.returncode == 1, blind.stderr
        assert "Permission denied" in blind.stderr, blind.stderr
    finally:
        shut.chmod(0o755)


def check_chmod_leaves_the_owner_question_to_the_system(data, workspace):
    """Whether a mode may change is the kernel's answer, reported per path.

    `/` is owned by root and read-only in the validation namespace. The kernel
    may report either restriction; the requested mode is its current mode."""
    if os.geteuid() == 0:
        return
    room = workspace / "owners"
    room.mkdir()
    mine = room / "mine.txt"
    mine.write_text("m")
    mine.chmod(0o200)
    root_mode = f"{mode_of('/') & 0o777:o}"

    # A dry run makes its own refusals and does not guess at the system's.
    guessed = run(data, "--dry-run", "chmod", root_mode, "/")
    assert guessed.returncode == 0, guessed.stderr
    assert guessed.stdout.endswith("\t/\n"), guessed.stdout

    denied = run(data, "chmod", root_mode, "/")
    assert denied.returncode == 1, denied.stdout
    assert any(f"chmod: changing permissions of /: {reason}" in denied.stderr
               for reason in ("Operation not permitted", "Read-only file system")), \
        denied.stderr

    # Every other operand is still changed, and the receipt names only those.
    both = run(data, "--json", "chmod", "u+r", "/", str(mine))
    assert both.returncode == 1, both.stderr
    assert mode_of(mine) == 0o600, oct(mode_of(mine))
    assert [row["path"] for row in json.loads(both.stdout)["modes"]] == [str(mine.resolve())]
    assert "changing permissions of /" in both.stderr, both.stderr


def check_ls_narrows_by_kind_and_mtime(data, workspace):
    """--type and --changed-within: what the file is and when it was written.

    Both narrow whatever the other flags chose -- the index rows, the walk of
    --untagged or --name -- and --changed-within reads the file's mtime, never
    the tag stamp --since reads."""
    room = workspace / "kinds-and-mtimes"
    folder = room / "folder"
    folder.mkdir(parents=True)
    bare = room / "bare"
    bare.mkdir()
    fresh = room / "fresh.txt"
    stale = room / "stale.txt"
    loose = room / "loose.txt"
    for path in (fresh, stale, loose):
        path.write_text("x")
    year_ago = time.time() - 400 * 86400
    os.utime(stale, (year_ago, year_ago))
    run(data, "tag", "add", "--tag", "shape", "--", str(fresh), str(stale), str(folder))

    def listed(*flags):
        done = run(data, "ls", *flags)
        assert done.returncode == 0, (flags, done.stderr)
        return done.stdout.splitlines()

    fresh, stale, loose, folder, bare = (str(p.resolve())
                                         for p in (fresh, stale, loose, folder, bare))
    assert listed("--tag", "shape") == [folder, fresh, stale]
    assert listed("--tag", "shape", "--type", "dir") == [folder]
    assert listed("--tag", "shape", "--type", "d") == [folder]
    assert listed("--tag", "shape", "--type", "file") == [fresh, stale]
    # Tagged just now, written a year ago: --since keeps it and this does not.
    assert listed("--tag", "shape", "--since", "1h") == [folder, fresh, stale]
    assert listed("--tag", "shape", "--changed-within", "30d") == [folder, fresh]
    assert listed("--tag", "shape", "--type", "f", "--changed-within", "30d") == [fresh]
    # The walks are narrowed the same way.
    untagged = listed("--untagged", "--type", "file", str(room))
    assert untagged == [loose], untagged
    assert listed("--untagged", "--type", "dir", str(room)) == [bare]
    assert listed("--name", "*.txt", "--changed-within", "30d", str(room)) == [fresh, loose]
    # And so is the list sweep built.
    home = workspace / "kinds-and-mtimes-home"
    env = {"XDG_CONFIG_HOME": str(home / "config"), "XDG_STATE_HOME": str(home / "state"),
           "XDG_CACHE_HOME": str(home / "cache")}
    assert run(data, "roots", "add", "--id", "kinds", "--", room, extra_env=env).returncode == 0
    try:
        assert run(data, "sweep", extra_env=env).returncode == 0
        cached = run(data, "ls", "--untagged", "--cached", "--type", "file", extra_env=env)
        assert cached.stdout.splitlines() == [loose], cached
    finally:
        # Undeclared, so the rows the sweep filed under it do not name a root
        # the checks after this one have never heard of.
        assert run(data, "roots", "rm", "--id", "kinds", extra_env=env).returncode == 0

    wrong = run(data, "ls", "--type", "link")
    assert wrong.returncode == 1 and "--type takes file or dir" in wrong.stderr, wrong.stderr
    # The one duration parser, so its refusal is --since's.
    bad = run(data, "ls", "--changed-within", "7x")
    assert bad.returncode == 1 and "invalid duration: 7x" in bad.stderr, bad.stderr
    # The next word is the value even when it spells a global, per POSIX
    # guideline 6, so neither flag lets `-q` turn into --quiet.
    assert "not -q" in run(data, "ls", "--type", "-q").stderr
    assert "invalid duration: -q" in run(data, "ls", "--changed-within", "-q").stderr
    flags = run(data, "__complete", "--", "ls", "-").stdout.split()
    assert "--type" in flags and "--changed-within" in flags, flags
    assert run(data, "__complete", "--", "ls", "--type", "").stdout.split() == ["dir", "file"]


def check_a_walk_lists_what_the_disk_holds(data, workspace):
    """The walk is what `find -P` sees: every entry by its own name.

    Fifos, sockets and dangling links are listed, and a link is listed as
    itself rather than its target, even one that leads out of the tree. The tag
    verbs still read a link as the file it leads to, since that is where a tag
    on it lives."""
    import socket
    room = workspace / "disk-holds"
    room.mkdir()
    real = room / "real.txt"
    real.write_text("r")
    os.mkfifo(room / "pipe")
    sock = socket.socket(socket.AF_UNIX)
    sock.bind(str(room / "sock"))
    sock.close()
    os.symlink("real.txt", room / "alias")
    os.symlink("nowhere", room / "dangling")
    os.symlink("/", room / "out")
    (room / "sub").mkdir()
    os.symlink("sub", room / "subalias")
    base = room.resolve()
    every = sorted(str(base / n) for n in
                   ("real.txt", "pipe", "sock", "alias", "dangling", "out", "sub", "subalias"))

    walked = run(data, "ls", "--name", "*", str(room))
    assert walked.returncode == 0, walked.stderr
    assert walked.stdout.splitlines() == every, walked.stdout
    assert run(data, "ls", "--untagged", str(room)).stdout.splitlines() == every
    # A fifo is found by name too.
    assert run(data, "ls", "--name", "pipe", str(room)).stdout.splitlines() == [str(base / "pipe")]
    # A link counts as what it leads to, and one leading nowhere is a file.
    files = run(data, "ls", "--name", "*", "--type", "f", str(room)).stdout.splitlines()
    assert files == sorted(str(base / n) for n in
                           ("real.txt", "pipe", "sock", "alias", "dangling")), files
    dirs = run(data, "ls", "--name", "*", "--type", "d", str(room)).stdout.splitlines()
    assert dirs == sorted(str(base / n) for n in ("out", "sub", "subalias")), dirs

    # The tag verbs: a file reached by a link and by its name is one row, and
    # links out of the tree or to nothing add none.
    os.setxattr(real, "user.omafiles.tags", b"heldhere")
    found = run(data, "verify", str(room))
    assert found.stdout.splitlines() == [f"unindexed\t{base / 'real.txt'}\txattr=heldhere"], \
        found.stdout
    rebuilt = run(data, "reindex", str(room))
    assert rebuilt.returncode == 0, rebuilt.stderr
    assert rebuilt.stdout.splitlines() == [str(base / "real.txt")], rebuilt.stdout
    assert run(data, "verify", str(room)).returncode == 0
    # And a link carries no tag of its own, so it is untagged.
    untagged = run(data, "ls", "--untagged", str(room)).stdout.splitlines()
    assert str(base / "alias") in untagged and str(base / "real.txt") not in untagged, untagged
    run(data, "tag", "clear", "--", str(real))

    if os.geteuid() == 0:
        return                               # root may open everything
    # A file reached by a link and by its name is one path to the tag verbs, so
    # a failure on it is said once.
    locked = room / "locked.txt"
    locked.write_text("l")
    os.symlink("locked.txt", room / "lockalias")
    locked.chmod(0o000)
    try:
        refused = run(data, "reindex", str(room))
        assert refused.returncode == 1, refused.stderr
        assert refused.stderr.count(str(base / "locked.txt")) == 1, refused.stderr
    finally:
        locked.chmod(0o644)
    shut = room / "shut"
    shut.mkdir()
    (shut / "inside.txt").write_text("i")
    shut.chmod(0o000)
    try:
        short = run(data, "ls", "--name", "*", str(room))
        assert short.returncode == 1, short.stderr
        assert str(base / "shut") not in short.stdout.splitlines(), \
            "a directory reported as unreadable was also listed"
        assert f"{base / 'shut'} is unreadable" in short.stderr, short.stderr
        rebuilt = run(data, "reindex", str(room))
        assert rebuilt.returncode == 1, rebuilt.stderr
        assert f"{base / 'shut'} is unreadable" in rebuilt.stderr, rebuilt.stderr
        assert rebuilt.stderr.count(str(base / "shut")) == 1, rebuilt.stderr
    finally:
        shut.chmod(0o755)


def check_a_short_list_of_names_says_so(data, workspace):
    """--cached out of a list sweep could not build whole is short, and says so.

    The walk says what it could not walk and exits 1; the list it builds has to
    remember the same, or the answer out of it reads as whole."""
    home = workspace / "short-list"
    tree = home / "tree"
    tree.mkdir(parents=True)
    env = {"XDG_CONFIG_HOME": str(home / "config"), "XDG_STATE_HOME": str(home / "state"),
           "XDG_CACHE_HOME": str(home / "cache")}
    (tree / "plain.txt").write_text("p")
    (tree / "two\nlines.txt").write_text("n")
    os.symlink("plain.txt", tree / "alias")
    os.mkfifo(tree / "pipe")
    assert run(data, "roots", "add", "--id", "shortlist", "--", str(tree),
               extra_env=env).returncode == 0
    try:
        assert run(data, "sweep", extra_env=env).returncode == 0
        answered = run(data, "ls", "--untagged", "--cached", extra_env=env)
        assert answered.returncode == 1, answered.stderr
        # The entries the walk lists, links by their own names.
        assert answered.stdout.splitlines() == [str(tree.resolve() / n)
                                                for n in ("alias", "pipe", "plain.txt")], \
            answered.stdout
        assert "the list of names is short: 1 name(s) hold a newline" in answered.stderr, \
            answered.stderr
        # --quiet takes the note and leaves the status.
        assert run(data, "--quiet", "ls", "--untagged", "--cached",
                   extra_env=env).returncode == 1
        (tree / "two\nlines.txt").unlink()
        if os.geteuid() != 0:
            shut = tree / "shut"
            shut.mkdir()
            shut.chmod(0o000)
            try:
                assert run(data, "sweep", extra_env=env).returncode == 0
            finally:
                shut.chmod(0o755)
            cached = run(data, "ls", "--name", "*", "--cached", extra_env=env)
            assert cached.returncode == 1, cached.stderr
            assert str(shut.resolve()) not in cached.stdout.splitlines(), cached.stdout
            assert f"{shut.resolve()} is unreadable; nothing under it was listed" \
                in cached.stderr, cached.stderr
            shut.rmdir()
        assert run(data, "sweep", extra_env=env).returncode == 0
        whole = run(data, "ls", "--untagged", "--cached", extra_env=env)
        assert whole.returncode == 0, whole.stderr
    finally:
        assert run(data, "roots", "rm", "--id", "shortlist", extra_env=env).returncode == 0


def check_every_printed_row_is_absolute(data, workspace):
    """Output is the absolute path; only `export` keeps the stored form.

    A row under a declared root is stored as the root and the rest of the way,
    and that form leaked into every verb's --json, which promises the absolute
    path."""
    home = workspace / "absolute-rows"
    tree = home / "tree"
    tree.mkdir(parents=True)
    env = {"XDG_CONFIG_HOME": str(home / "config"), "XDG_STATE_HOME": str(home / "state"),
           "XDG_CACHE_HOME": str(home / "cache")}
    base = tree.resolve()
    first = tree / "first.txt"
    first.write_text("f")
    assert run(data, "roots", "add", "--id", "absrows", "--", str(tree),
               extra_env=env).returncode == 0

    def rows(*args, stdin=None):
        done = run(data, "--json", *args, extra_env=env, stdin=stdin)
        assert done.returncode == 0, (args, done.stderr)
        value = json.loads(done.stdout)
        found = value if isinstance(value, list) else value.get("rows", [value])
        for row in found:
            assert "root" not in row and row["path"].startswith("/"), (args, row)
        return found

    try:
        assert rows("tag", "add", "--tag", "abs", "--", str(first))
        assert rows("tag", "ls", "--", str(first))
        assert rows("json", "--", str(first))
        assert rows("ls", "--tag", "abs")
        assert rows("recent")
        assert rows("mv", str(first), str(tree / "moved.txt"))
        assert rows("cp", str(tree / "moved.txt"), str(tree / "copied.txt"))
        assert rows("batch", "-", stdin=f"tag add --tag more -- {tree / 'copied.txt'}\n")
        assert rows("--dry-run", "tag", "add", "--tag", "dry", "--", str(tree / "copied.txt"))
        dump = run(data, "export", extra_env=env).stdout
        mine = [json.loads(line) for line in dump.splitlines() if "absrows" in line]
        assert {row["path"] for row in mine} == {"moved.txt", "copied.txt"}, mine
        # Taken off so the import changes a row, and has one to print.
        assert run(data, "tag", "rm", "--tag", "more", "--", tree / "copied.txt",
                   extra_env=env).returncode == 0
        assert rows("import", "-", stdin=dump)
        os.setxattr(tree / "copied.txt", "user.omafiles.tags", b"abs,fromfile")
        assert rows("reindex", "--from-xattr", str(tree))
        assert rows("tag", "clear", "--", str(tree / "moved.txt"), str(tree / "copied.txt"))
    finally:
        assert run(data, "roots", "rm", "--id", "absrows", extra_env=env).returncode == 0


def check_chmod_takes_each_path_once(data, workspace):
    """Two spellings of one path are one operand, and each old mode is fresh:
    `chmod u+x a a ./a` changes and reports the file once."""
    room = workspace / "chmod-once"
    room.mkdir()
    one = room / "one.txt"
    one.write_text("o")
    one.chmod(0o644)
    done = run(data, "chmod", "u+x", str(one), str(one), f"{room}/./one.txt")
    assert done.returncode == 0, done.stderr
    assert done.stdout.splitlines() == [f"0644\t0744\t{one.resolve()}"], done.stdout
    # A second name for the same inode reports what the first change left.
    twin = room / "twin.txt"
    os.link(one, twin)
    both = run(data, "chmod", "go-r", str(one), str(twin))
    assert both.stdout.splitlines() == [f"0744\t0700\t{one.resolve()}",
                                        f"0700\t0700\t{twin.resolve()}"], both.stdout
    # The same rule for every verb that resolves its operands: one receipt row.
    tagged = run(data, "--json", "tag", "add", "--tag", "once", "--", str(one), f"{room}/./one.txt")
    assert [row["path"] for row in json.loads(tagged.stdout)] == [str(one.resolve())], tagged.stdout
    run(data, "tag", "clear", "--", str(one))
    # Leading zeros, as coreutils takes them; a special bit is still refused.
    assert run(data, "chmod", "00644", str(one)).returncode == 0
    assert mode_of(one) & 0o7777 == 0o644
    assert run(data, "chmod", "0000600", str(one)).returncode == 0
    assert mode_of(one) & 0o7777 == 0o600
    special = run(data, "chmod", "004755", str(one))
    assert special.returncode == 1 and "setuid, setgid or sticky" in special.stderr, special.stderr
    assert "invalid mode" in run(data, "chmod", "17777", str(one)).stderr
    # The refusal says what is true of every link: a loop, a dangling one.
    os.symlink("loop", room / "loop")
    looped = run(data, "chmod", "644", str(room / "loop"))
    assert looped.returncode == 1, looped.stderr
    assert f"{room.resolve()}/loop is a symlink; chmod acts on names here and refuses links" \
        in looped.stderr, looped.stderr


def check_a_shape_needs_a_path_it_can_read(data, workspace):
    """--type and --changed-within drop a path they cannot describe.

    A file deleted since the sweep is not listed under `--cached --type f`,
    though `QFileInfo::isDir()` is false for a path that is not there."""
    home = workspace / "shape-gone"
    tree = home / "tree"
    tree.mkdir(parents=True)
    env = {"XDG_CONFIG_HOME": str(home / "config"), "XDG_STATE_HOME": str(home / "state"),
           "XDG_CACHE_HOME": str(home / "cache")}
    kept = tree / "kept.txt"
    gone = tree / "gone.txt"
    kept.write_text("k")
    gone.write_text("g")
    assert run(data, "roots", "add", "--id", "shapegone", "--", str(tree),
               extra_env=env).returncode == 0
    try:
        assert run(data, "sweep", extra_env=env).returncode == 0
        gone.unlink()
        # Without a shape nothing is asked of the disk: the list's answer stands.
        stale = run(data, "ls", "--untagged", "--cached", extra_env=env).stdout.splitlines()
        assert str(gone.resolve()) in stale, stale
        for kind in ("f", "d"):
            shaped = run(data, "ls", "--untagged", "--cached", "--type", kind, extra_env=env)
            assert str(gone.resolve()) not in shaped.stdout.splitlines(), (kind, shaped.stdout)
        filed = run(data, "ls", "--untagged", "--cached", "--type", "f", extra_env=env)
        assert filed.stdout.splitlines() == [str(kept.resolve())], filed.stdout
    finally:
        assert run(data, "roots", "rm", "--id", "shapegone", extra_env=env).returncode == 0


def check_a_name_pattern_is_fnmatch(data, workspace):
    """--name is fnmatch(3), folded: a backslash escapes, a lone `[` is itself.

    Qt's wildcard translation ignored the escape and turned a lone `[` into a
    regular expression that matched nothing."""
    room = workspace / "fnmatch"
    room.mkdir()
    for name in ("[", "a*b", "axb", "Loud.PDF"):
        (room / name).write_text("x")
    base = room.resolve()

    def named(pattern):
        done = run(data, "ls", "--name", pattern, str(room))
        assert done.returncode == 0, (pattern, done.stderr)
        return done.stdout.splitlines()

    assert named("[") == [str(base / "[")]
    assert named("a\\*b") == [str(base / "a*b")]
    assert named("a*b") == [str(base / "a*b"), str(base / "axb")]
    assert named("[ab]x?") == [str(base / "axb")]
    assert named("*.pdf") == [str(base / "Loud.PDF")]


def check_ext_refuses_what_no_suffix_is(data, workspace):
    """--ext is what follows the last dot, so no value holding one can match."""
    for value in ("", ".", "tar.gz", "*", "a/b", "..pdf"):
        refused = run(data, "ls", "--ext", value)
        assert refused.returncode == 1, (value, refused.stdout)
        assert "--ext takes a suffix" in refused.stderr, (value, refused.stderr)
        assert refused.stdout == ""
    assert run(data, "ls", "--ext", ".PDF").returncode == 0


def check_a_stamp_before_1970_is_malformed(data, workspace):
    """A `tagged` of zero or below is a row nothing here wrote, like `true`."""
    room = workspace / "stamp-1970"
    room.mkdir()
    for stamp in ("0", "-5"):
        dump = room / f"dump{stamp}.jsonl"
        dump.write_text(json.dumps({"path": str(room / "x"), "tags": ["t"],
                                    "tagged": int(stamp)}) + "\n")
        refused = run(data, "import", str(dump))
        assert refused.returncode == 1, refused.stderr
        assert "line 1" in refused.stderr and "tagged" in refused.stderr, refused.stderr
    broken = room / "xdg"
    (broken / "omafiles").mkdir(parents=True)
    (broken / "omafiles" / "index.jsonl").write_text(
        json.dumps({"path": "/x", "tags": ["t"], "tagged": 0}) + "\n")
    refused = run(broken, "ls")
    assert refused.returncode == 3, refused.stderr
    assert f"store: malformed jsonl at {broken}/omafiles/index.jsonl:1: index row has a " \
        "tagged field" in refused.stderr, refused.stderr


def run_bytes(env_home, *args, extra_env=None, stdin=b"", shell=None):
    """run(), with every word and every value handed over as bytes.

    `run` goes through text, which cannot carry a byte that is not UTF-8, and
    those bytes are the whole point of the checks that use this."""
    env = {os.fsencode(k): os.fsencode(v) for k, v in
           {**os.environ, **sandbox(env_home / "data", env_home)}.items()}
    env.update(extra_env or {})
    words = [os.fsencode(word) if isinstance(word, (str, Path)) else word for word in args]
    command = [b"sh", b"-c", shell, os.fsencode(CLI), *words] if shell \
        else [os.fsencode(CLI), *words]
    return subprocess.run(command, cwd=str(ROOT), env=env, input=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def check_bytes_that_are_not_utf8_name_no_other_file(data, workspace):
    """A word that is not UTF-8 is refused, never decoded into U+FFFD.

    Qt decodes `\\xff\\xfe` as two U+FFFD, which is the name of the file beside
    it, so `rm --dry-run` named that sibling and `chmod` changed it. coreutils
    works on bytes; a path here is text and cannot, so the word is refused."""
    home = workspace / "not-utf8"
    room = home / "room"
    room.mkdir(parents=True)
    bad = os.fsencode(room) + b"/\xff\xfe"
    with open(bad, "w") as handle:
        handle.write("b")
    sibling = room / "��"
    sibling.write_text("s")
    sibling.chmod(0o644)
    plain = room / "a"
    plain.write_text("a")

    for words in ((b"rm", b"--dry-run", bad), (b"chmod", b"600", bad),
                  (b"tag", b"add", b"--tag", b"x", b"--", bad),
                  (b"ls", b"--name", b"\xff*")):
        done = run_bytes(home, *words)
        assert done.returncode == 1, (words, done.returncode, done.stdout, done.stderr)
        assert b"\\xff" in done.stderr and b"not valid UTF-8" in done.stderr, done.stderr
        assert done.stdout == b"", done.stdout
    assert sibling.stat().st_mode & 0o777 == 0o644, oct(sibling.stat().st_mode)
    assert "�" not in run_bytes(home, "export").stdout.decode()

    # A batch line is the same word, read from a file; and a NUL in one would
    # name only the part before it.
    for line in (b"tag add --tag x -- " + bad, b"tag add --tag x -- " + os.fsencode(plain)
                 + b"\x00tail"):
        batched = run_bytes(home, "batch", stdin=line + b"\n")
        assert batched.returncode == 1, (line, batched.stderr)
        assert b"batch: line 1" in batched.stderr, batched.stderr
    assert run_bytes(home, "export").stdout == b""

    # The directories every file is found through. A default is decided by
    # `$HOME`, so that is one of them too.
    shadow = home / "�"
    (shadow / "omafiles").mkdir(parents=True)
    (shadow / "omafiles" / "index.jsonl").write_text(
        json.dumps({"path": str(plain), "tags": ["shadow"]}) + "\n")
    for variable in (b"XDG_DATA_HOME", b"XDG_CONFIG_HOME", b"XDG_CACHE_HOME",
                     b"XDG_STATE_HOME"):
        done = run_bytes(home, "ls", extra_env={variable: os.fsencode(home) + b"/\xff"})
        assert done.returncode == 1, (variable, done.stdout, done.stderr)
        assert variable in done.stderr and b"\\xff" in done.stderr, done.stderr
        assert done.stdout == b"", done.stdout
    unset = run_bytes(home, "ls", extra_env={b"XDG_DATA_HOME": b"",
                                             b"HOME": os.fsencode(home) + b"/\xff"})
    assert unset.returncode == 1 and b"HOME" in unset.stderr, unset.stderr

    # The list of names is written by `sweep` and read back the same way.
    names = home / "cache" / "omafiles" / "names" / "room"
    assert run_bytes(home, "roots", "add", "--id", "room", "--", room).returncode == 0
    names.parent.mkdir(parents=True)
    names.write_bytes(b"a\n\xff\xfe\n")
    cached = run_bytes(home, "ls", "--untagged", "--cached")
    assert cached.returncode == 1 and b"not valid UTF-8" in cached.stderr, cached.stderr
    assert "\ufffd" not in cached.stdout.decode(errors="strict"), cached.stdout
    assert run_bytes(home, "roots", "rm", "--id", "room").returncode == 0

    # And the desktop's list spells a path in percent-encoded bytes.
    (home / "data").mkdir(exist_ok=True)
    (home / "data" / "recently-used.xbel").write_text(f"""<?xml version="1.0"?>
<xbel version="1.0">
  <bookmark href="file://{room}/%FF%FE" visited="2026-09-02T10:00:00Z"/>
  <bookmark href="file://{plain}" visited="2026-09-01T10:00:00Z"/>
</xbel>
""")
    opened = run_bytes(home, "recent", "--desktop")
    assert opened.returncode == 0, opened.stderr
    assert opened.stdout.decode().splitlines() == [str(plain)], opened.stdout


def check_could_not_look_is_not_absent(data, workspace):
    """Only ENOENT is "not there"; every other failure is said.

    An index directory chmod'd shut made `ls`, `tags` and `recent` print nothing
    and exit 0, because `QFile::exists` is false for a file nobody may look at
    as well as for one that is not there. The same question was asked of the
    roots, the list of names, the sweep receipt, the desktop's list, a root's
    own check and a destination."""
    home = workspace / "could-not-look"
    room = home / "room"
    room.mkdir(parents=True)
    subject = room / "s.txt"
    subject.write_text("s")
    assert run_bytes(home, "tag", "add", "--tag", "seen", "--", subject).returncode == 0
    denied = os.strerror(13).encode()

    def shut(path):
        path.mkdir(parents=True, exist_ok=True)
        path.chmod(0o000)

    try:
        shut(home / "data" / "omafiles")
        for words in (("ls",), ("--json", "ls"), ("tags",), ("recent",)):
            done = run_bytes(home, *words)
            assert done.returncode == 3, (words, done.returncode, done.stdout, done.stderr)
            assert b"store: cannot read" in done.stderr and denied in done.stderr, done.stderr
            assert done.stdout == b"", done.stdout
    finally:
        (home / "data" / "omafiles").chmod(0o755)

    try:
        shut(home / "config" / "omafiles")
        listed = run_bytes(home, "roots")
        assert listed.returncode == 3, (listed.returncode, listed.stdout, listed.stderr)
        assert b"roots: cannot read" in listed.stderr and denied in listed.stderr, listed.stderr
    finally:
        (home / "config" / "omafiles").chmod(0o755)

    assert run_bytes(home, "roots", "add", "--id", "room", "--", room).returncode == 0
    assert run_bytes(home, "sweep").returncode == 0
    try:
        shut(home / "cache" / "omafiles" / "names")
        cached = run_bytes(home, "ls", "--untagged", "--cached")
        assert cached.returncode == 1, (cached.returncode, cached.stderr)
        assert b"names: cannot read" in cached.stderr and denied in cached.stderr, cached.stderr
    finally:
        (home / "cache" / "omafiles" / "names").chmod(0o755)

    receipt = home / "state" / "omafiles" / "sweep.json"
    try:
        receipt.parent.chmod(0o000)
        status = run_bytes(home, "sweep", "--status")
        assert status.returncode == 1 and b"never" not in status.stdout, status
        assert denied in status.stderr, status.stderr
    finally:
        receipt.parent.chmod(0o755)
    # Read in the shape a sweep writes it, as every kept file is.
    kept = receipt.read_bytes()
    for body in (b"not json", b"\xef\xbb\xbf" + kept):
        receipt.write_bytes(body)
        status = run_bytes(home, "sweep", "--status")
        assert status.returncode == 1 and b"never" not in status.stdout, (body, status)
        assert b"is not a receipt" in status.stderr, status.stderr
    receipt.unlink()
    assert run_bytes(home, "sweep", "--status").stdout == b"never\n"

    # A root whose check nobody may look at is neither here nor away, and the
    # rows under it are not listed as if it were either.
    assert run_bytes(home, "roots", "rm", "--id", "room").returncode == 0
    marker = room / ".here"
    marker.write_text("")
    assert run_bytes(home, "roots", "add", "--id", "room", "--check", ".here",
                     "--", room).returncode == 0
    (home / "data" / "recently-used.xbel").write_text(f"""<?xml version="1.0"?>
<xbel version="1.0"><bookmark href="file://{subject}" visited="2026-09-01T10:00:00Z"/></xbel>
""")
    try:
        room.chmod(0o000)
        listed = run_bytes(home, "roots")
        assert listed.returncode == 1, (listed.returncode, listed.stderr)
        assert listed.stdout.decode().split("\t")[-1] == "unknown\n", listed.stdout
        assert listed.stderr.startswith(b"roots ls: room cannot be checked: ") \
            and denied in listed.stderr, listed.stderr
        assert json.loads(run_bytes(home, "--json", "roots").stdout)[0]["here"] is None
        for words in (("ls",), ("tags",), ("recent",), ("recent", "--desktop")):
            done = run_bytes(home, *words)
            assert done.returncode == 1, (words, done.returncode, done.stdout, done.stderr)
            assert str(subject).encode() in done.stderr and denied in done.stderr, done.stderr
            assert b"s.txt" not in done.stdout and b"seen" not in done.stdout, done.stdout
        swept = run_bytes(home, "sweep")
        assert swept.returncode == 0 and b"cannot be checked" in swept.stderr, swept.stderr
        cached = run_bytes(home, "ls", "--untagged", "--cached")
        assert cached.returncode == 1 and b"cannot be checked" in cached.stderr, cached.stderr
    finally:
        room.chmod(0o755)

    # A destination nobody may look at is not a free one, dry run or not.
    closed = home / "closed"
    closed.mkdir()
    try:
        closed.chmod(0o600)
        for verb in ("mv", "cp"):
            done = run_bytes(home, "--dry-run", verb, subject, closed / "t.txt")
            assert done.returncode == 1, (verb, done.returncode, done.stdout, done.stderr)
            assert denied in done.stderr, done.stderr
    finally:
        closed.chmod(0o755)


def check_not_a_directory_is_the_systems_answer(data, workspace):
    """`a/b` with `a` a file is exit 1 and `Not a directory`, as coreutils says.

    It is not `path missing`: nothing asked about `b`, the system stopped at
    `a`, and exit 2 would send somebody looking for a file to create."""
    home = workspace / "not-a-directory"
    home.mkdir()
    plain = home / "plain"
    plain.write_text("p")
    for words in (("tag", "add", "--tag", "x", "--"), ("rm", "--dry-run", "--"),
                  ("tag", "ls", "--")):
        done = run_bytes(home, *words, plain / "inside")
        assert done.returncode == 1, (words, done.returncode, done.stderr)
        assert os.strerror(20).encode() in done.stderr, done.stderr
        assert b"path missing" not in done.stderr, done.stderr


def check_a_dangling_chain_names_its_missing_end(data, workspace):
    """A link to a link to nothing names every hop, not just the first."""
    home = workspace / "dangling-chain"
    home.mkdir()
    (home / "la").symlink_to(home / "lb")
    (home / "lb").symlink_to(home / "nowhere")
    done = run_bytes(home, "tag", "ls", "--", home / "la")
    assert done.returncode == 2, done.stderr
    said = f"{home}/la is a symlink to {home}/lb, which is a symlink to {home}/nowhere, " \
           "which is not there"
    assert said.encode() in done.stderr, done.stderr


def check_an_away_root_wins_over_the_disk(data, workspace):
    """A root that is away leaves its rows out of every list, stat or no stat.

    Files left in an unmounted mount point are not the storage; the declaration
    says the storage is not here, so what answers at that path is not it."""
    home = workspace / "away-wins"
    disk = home / "disk"
    disk.mkdir(parents=True)
    marker = disk / ".here"
    marker.write_text("")
    stray = disk / "f.txt"
    stray.write_text("f")
    assert run_bytes(home, "roots", "add", "--id", "disk", "--check", ".here",
                     "--", disk).returncode == 0
    assert run_bytes(home, "tag", "add", "--tag", "d", "--", stray).returncode == 0
    marker.unlink()
    assert run_bytes(home, "roots").stdout.decode().endswith("\taway\n")
    for words in (("ls",), ("ls", "--tag", "d"), ("tags",), ("recent",)):
        done = run_bytes(home, *words)
        assert done.returncode == 1, (words, done.returncode, done.stdout, done.stderr)
        assert b"root disk is declared and not here" in done.stderr, done.stderr
        assert done.stdout == b"", done.stdout
    # Nor is its mount point somewhere to put a file.
    elsewhere = home / "elsewhere.txt"
    elsewhere.write_text("e")
    moved = run_bytes(home, "--dry-run", "mv", elsewhere, disk / "e.txt")
    assert moved.returncode == 1, (moved.returncode, moved.stdout, moved.stderr)
    assert b"root disk is declared and not here" in moved.stderr, moved.stderr


def check_a_write_error_names_its_own_reason(data, workspace):
    """The reason is the failing write's, not whatever errno held at exit.

    With stdout closed it said `Invalid argument`, Qt's word for a stream on a
    descriptor that is not there. A closed standard descriptor is opened on
    /dev/null the wrong way round, gnulib's `stdopen`, so the write fails with
    the EBADF it is and no file this run opens can take descriptor 1."""
    home = workspace / "write-reason"
    home.mkdir()
    subject = home / "w.txt"
    subject.write_text("w")
    closed = run_bytes(home, "--json", "tag", "add", "--tag", "w", "--", subject,
                       shell=b'exec "$0" "$@" >&-')
    assert closed.returncode == 1, closed
    assert f"tag add: write error: {os.strerror(9)}".encode() in closed.stderr, closed.stderr
    full = run_bytes(home, "ls", shell=b'exec "$0" "$@" 2>&1 >/dev/full')
    assert f"ls: write error: {os.strerror(28)}".encode() in full.stdout, full

    # A write that fails part way, with the run going on to ask the system
    # other things: errno at exit is the last of those, not the write's.
    rows = "".join(json.dumps({"path": f"{home}/gone-with-a-long-enough-name-{n}.txt",
                               "tags": ["gone"]}) + "\n" for n in range(1500))
    (home / "data" / "omafiles" / "index.jsonl").write_text(rows)
    late = run_bytes(home, "verify", home, shell=b'exec "$0" "$@" 2>&1 >/dev/full')
    assert f"verify: write error: {os.strerror(28)}".encode() in late.stdout, late.stdout


def check_a_row_under_a_file_is_gone(data, workspace):
    """A row whose directory became a file is gone, not unreadable.

    ENOTDIR says nothing can be at the path while that file stands there, so
    `verify` calls the row missing, `sweep` counts it gone and `prune` drops it
    -- the two errnos coreutils `rm -f` treats as a missing file. An operand
    still gets the system's own answer; see
    check_not_a_directory_is_the_systems_answer."""
    home = workspace / "row-under-a-file"
    room = home / "room"
    folder = room / "folder"
    folder.mkdir(parents=True)
    subject = folder / "inside.txt"
    subject.write_text("i")
    assert run_bytes(home, "tag", "add", "--tag", "x", "--", subject).returncode == 0
    subject.unlink()
    folder.rmdir()
    folder.write_text("a file where the folder was")
    row = str(subject).encode()

    checked = run_bytes(home, "verify", room)
    assert checked.returncode == 1, checked.stderr
    assert checked.stdout.splitlines() == [b"missing\t" + row], checked.stdout
    swept = run_bytes(home, "--json", "sweep", room)
    assert json.loads(swept.stdout)["gone"] == [str(subject)], (swept.stdout, swept.stderr)
    pruned = run_bytes(home, "--yes", "prune", room)
    assert pruned.returncode == 0, pruned.stderr
    assert pruned.stdout.splitlines() == [row], pruned.stdout
    assert run_bytes(home, "export").stdout == b""


def landlocked_readdir():
    """A preexec_fn denying every directory listing through Landlock, or None.

    `access(2)` is not a Landlock hook, so the directory still reads as
    readable to anything that asks instead of opening it."""
    libc = ctypes.CDLL(None, use_errno=True)
    create, restrict, read_dir, no_new_privs = 444, 446, 1 << 3, 38
    if libc.syscall(create, None, 0, 1) < 1:        # the ABI version; none here
        return None

    class Ruleset(ctypes.Structure):
        _fields_ = [("handled_access_fs", ctypes.c_uint64)]

    def deny():
        ruleset = Ruleset(read_dir)
        fd = libc.syscall(create, ctypes.byref(ruleset), ctypes.sizeof(ruleset), 0)
        if fd < 0 or libc.prctl(no_new_privs, 1, 0, 0, 0) != 0 \
                or libc.syscall(restrict, fd, 0) != 0:
            os._exit(99)
        os.close(fd)
    return deny


def check_a_directory_is_shut_when_it_will_not_open(data, workspace):
    """Whether `[dir]` can be looked into is the answer of opening it.

    `access(2)` is a guess: where it says yes and the open says no, a verb
    asking it would answer for a tree it cannot see."""
    deny = landlocked_readdir()
    if deny is None:
        return
    room = workspace / "will-not-open"
    room.mkdir()
    (room / "f.txt").write_text("f")
    assert run(data, "tag", "add", "--tag", "shutroom", "--", room / "f.txt").returncode == 0
    for words, done in ((("sweep",), "swept"),):
        answered = run(data, *words, room, preexec_fn=deny)
        assert answered.returncode == 1, (words, answered.returncode, answered.stderr)
        assert f"{room.resolve()} is unreadable; nothing under it was {done}" \
            in answered.stderr, (words, answered.stderr)
    assert run(data, "tag", "clear", "--", room / "f.txt").returncode == 0


def check_the_window_refuses_what_it_cannot_spell(data, workspace):
    """The window keeps the command line's rule on bytes that are not UTF-8.

    Decoded, `\\xff\\xfe` names the folder beside it, and a base directory
    spelled so names some other one."""
    studio = CLI.parent / "omafiles-studio"
    if not studio.is_file():
        return
    home = workspace / "window-not-utf8"
    (home / "\ufffd\ufffd").mkdir(parents=True)
    bad = os.fsencode(home) + b"/\xff\xfe"
    os.mkdir(bad)
    env = {**cli_env(data), "QT_QPA_PLATFORM": "offscreen"}
    for words, extra in (([bad], {}),
                         ([], {b"XDG_STATE_HOME": os.fsencode(home) + b"/\xff"})):
        try:
            done = subprocess.run([os.fsencode(studio), *words], cwd=ROOT,
                                  env={**{os.fsencode(k): os.fsencode(v)
                                          for k, v in env.items()}, **extra},
                                  stdin=subprocess.DEVNULL, capture_output=True, timeout=20)
        except subprocess.TimeoutExpired:
            raise AssertionError(f"the window opened for {words} {extra}")
        assert done.returncode == 1, (words, extra, done.returncode, done.stderr)
        assert b"\\xff" in done.stderr and b"not valid UTF-8" in done.stderr, done.stderr


def fresh(workspace, name):
    """A home of its own for one check: XDG_DATA_HOME, and the rest under it."""
    home = workspace / name
    (home / "w").mkdir(parents=True)
    return home, home / "w"


def write_index(home, rows):
    (home / "omafiles").mkdir(parents=True, exist_ok=True)
    (home / "omafiles" / "index.jsonl").write_text(
        "".join(json.dumps(row) + "\n" for row in rows))


def check_recent_orders_before_it_looks(data, workspace):
    """`recent` orders by the stamps it holds, then looks, down to the limit.

    A row below the limit that nobody may look at is not in the answer, so it
    cannot cost it; a row whose directory is now a file is gone."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r2b-recent")
    (w / "new.txt").write_text("n")
    (w / "shut").mkdir()
    (w / "shut" / "old.txt").write_text("o")
    (w / "p").write_text("a file where a folder was")
    write_index(home, [
        {"path": str(w / "new.txt"), "tags": ["n"], "tagged": 300},
        {"path": str(w / "p" / "gone.txt"), "tags": ["g"], "tagged": 250},
        {"path": str(w / "shut" / "old.txt"), "tags": ["o"], "tagged": 100}])
    (w / "shut").chmod(0o000)
    try:
        top = run(home, "recent", "1")
        assert top.returncode == 0, (top.returncode, top.stderr)
        assert top.stdout.splitlines() == [str(w / "new.txt")], top.stdout
        assert top.stderr == "", top.stderr
        both = run(home, "recent", "2")
        assert both.returncode == 1 and "old.txt" in both.stderr, (both.returncode, both.stderr)
        assert "gone.txt" not in both.stderr, both.stderr
        assert both.stdout.splitlines() == [str(w / "new.txt")], both.stdout
    finally:
        (w / "shut").chmod(0o755)


def check_recent_desktop_is_one_row_per_file(data, workspace):
    """`recent --desktop` resolves each entry, keeps the newest, reads
    `file://localhost` as local, and orders before it looks."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r2b-desktop")
    (w / "d").mkdir()
    (w / "a.md").write_text("a")
    (w / "f").write_text("a file")
    (w / "shut").mkdir()
    (w / "shut" / "s.md").write_text("s")
    assert run(home, "tag", "add", "--tag", "work", "--", w / "a.md").returncode == 0
    xbel = home / "recently-used.xbel"
    xbel.write_text(f"""<?xml version="1.0"?><xbel version="1.0">
<bookmark href="file://{w}/d/../a.md" visited="2026-09-05T10:00:00Z"/>
<bookmark href="file://{w}/f/under-a-file.md" visited="2026-09-04T10:00:00Z"/>
<bookmark href="file://localhost{w}/a.md" visited="2026-09-03T10:00:00Z"/>
<bookmark href="file://{w}//a.md" visited="2026-09-02T10:00:00Z"/>
<bookmark href="file://{w}/shut/s.md" visited="2026-09-01T10:00:00Z"/>
</xbel>
""")
    (w / "shut").chmod(0o000)
    try:
        one = run(home, "--json", "recent", "--desktop", "1")
        assert one.returncode == 0, (one.returncode, one.stderr)
        assert [(row["path"], row["tags"]) for row in json.loads(one.stdout)] \
            == [(str(w / "a.md"), ["work"])], one.stdout
        whole = run(home, "recent", "--desktop")
        assert whole.returncode == 1 and "s.md" in whole.stderr, (whole.returncode, whole.stderr)
        assert "under-a-file" not in whole.stderr, whole.stderr
        assert whole.stdout.splitlines() == [str(w / "a.md")], whole.stdout
    finally:
        (w / "shut").chmod(0o755)
    only_localhost = home / "recently-used.xbel"
    only_localhost.write_text(f"""<?xml version="1.0"?><xbel version="1.0">
<bookmark href="file://localhost{w}/a.md" visited="2026-09-03T10:00:00Z"/></xbel>
""")
    local = run(home, "recent", "--desktop")
    assert local.stdout.splitlines() == [str(w / "a.md")], (local.stdout, local.stderr)
    xbel.write_bytes(b"")
    empty = run(home, "--json", "recent", "--desktop")
    assert empty.returncode == 0 and json.loads(empty.stdout) == [], (empty.stdout, empty.stderr)


def shut_gio():
    """A bwrap prefix with `gio` shut, and what `trash` then says, or None.

    PATH is left alone: a PATH without `timeout` or `sh` fails as something
    else entirely. `gio` stays on PATH as a file nobody may run."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        return None
    found = shutil.which("gio")
    if not found:
        return [bwrap, "--dev-bind", "/", "/"], "gio is not installed"
    return ([bwrap, "--dev-bind", "/", "/", "--ro-bind", "/dev/null", os.path.realpath(found)],
            "cannot be run")


def recording_openers(log):
    """A bwrap prefix putting a recorder of its first argument, appended to
    `log`, in place of every program `open` launches, with `xdg-mime` and `gio`
    shut, or None where bwrap or xdg-open is not installed. PATH is left alone."""
    bwrap = shutil.which("bwrap")
    if not bwrap or not shutil.which("xdg-open"):
        return None
    recorder = log.parent / "recorder"
    recorder.write_text(f'#!/bin/sh\necho "$1" >> {log}\n')
    recorder.chmod(0o755)
    binds = []
    for name, stand_in in (("xdg-open", recorder), ("omarchy-launch-editor", recorder),
                           ("xdg-mime", "/dev/null"), ("gio", "/dev/null")):
        found = shutil.which(name)
        if found:
            binds += ["--ro-bind", str(stand_in), os.path.realpath(found)]
    return [bwrap, "--dev-bind", "/", "/", *binds]


def hidden_openers():
    """A bwrap prefix that hides every program `open` could use, or None."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        return None
    hide = []
    for name in ("xdg-open", "gio", "xdg-mime", "omarchy-launch-editor"):
        found = shutil.which(name)
        if found:
            hide += ["--ro-bind", "/dev/null", os.path.realpath(found)]
    return [bwrap, "--dev-bind", "/", "/", *hide]


def check_open_names_every_path_before_the_opener(data, workspace):
    """Every operand is resolved before the opener is asked about, so every
    missing one is named; the dry run's line pastes back into a shell; and a
    path under an away root is refused."""
    import shlex
    home, w = fresh(workspace, "r2b-open")
    here = w / "it's here.txt"
    here.write_text("x")
    dry = run(home, "--dry-run", "open", here)
    assert dry.returncode == 0, dry.stderr
    assert shlex.split(dry.stdout)[-1] == str(here), dry.stdout

    rooted = w / "disk"
    rooted.mkdir()
    (rooted / ".here").write_text("")
    (rooted / "f.txt").write_text("f")
    assert run(home, "roots", "add", "--id", "disk", "--check", ".here", "--",
               rooted).returncode == 0
    (rooted / ".here").unlink()
    away = run(home, "--dry-run", "open", rooted / "f.txt")
    assert away.returncode == 1 and "declared and not here" in away.stderr, \
        (away.returncode, away.stderr)

    prefix = hidden_openers()
    if prefix is None:
        return
    env = cli_env(home)
    mime = shutil.which("xdg-mime")
    if mime:
        unsaid = subprocess.run([prefix[0], "--dev-bind", "/", "/", "--ro-bind", "/dev/null",
                                 os.path.realpath(mime), str(CLI), "--dry-run", "--json",
                                 "open", str(here)],
                                env=env, text=True, capture_output=True, stdin=subprocess.DEVNULL)
        if unsaid.returncode == 0:
            assert json.loads(unsaid.stdout)[0]["mime"] == "", unsaid.stdout
            assert "could not say" in unsaid.stderr, unsaid.stderr
    done = subprocess.run([*prefix, str(CLI), "--dry-run", "open", str(here),
                           str(w / "nothere1"), str(w / "nothere2")],
                          env=env, text=True, capture_output=True, stdin=subprocess.DEVNULL)
    if "no xdg-open" not in subprocess.run(
            [*prefix, str(CLI), "--dry-run", "open", str(here)], env=env, text=True,
            capture_output=True, stdin=subprocess.DEVNULL).stderr:
        return
    assert done.returncode == 2, (done.returncode, done.stderr)
    assert "nothere1" in done.stderr and "nothere2" in done.stderr, done.stderr


def check_a_tag_verb_starts_from_the_files_own_tags(data, workspace):
    """A path with no row whose file carries tags starts from them."""
    home, w = fresh(workspace, "r2b-adopt")
    probe = w / "probe"
    probe.write_text("p")
    try:
        os.setxattr(probe, "user.omafiles.tags", b"x")
    except OSError:
        return
    def carrying(name, value):
        path = w / name
        path.write_text(name)
        os.setxattr(path, "user.omafiles.tags", value)
        return path
    def row(path):
        got = run(home, "json", "--", path)
        return json.loads(got.stdout)["tags"] if got.stdout else None

    added = carrying("add.txt", b"foo,bar")
    assert run(home, "tag", "add", "--tag", "z", "--", added).returncode == 0
    assert row(added) == ["bar", "foo", "z"]
    assert os.getxattr(added, "user.omafiles.tags") == b"bar,foo,z"

    removed = carrying("rm.txt", b"foo,bar")
    gone = run(home, "--json", "tag", "rm", "--tag", "foo", "--", removed)
    assert gone.returncode == 0, gone.stderr
    assert [r["tags"] for r in json.loads(gone.stdout)] == [["bar"]], gone.stdout
    assert os.getxattr(removed, "user.omafiles.tags") == b"bar"

    source = carrying("src.txt", b"sx")
    target = carrying("dst.txt", b"dy")
    assert run(home, "tag", "cp", "--from", source, "--", target).returncode == 0
    assert row(target) == ["dy", "sx"]

    # A value holding a tag that is not one is left as it is, and said.
    mixed = carrying("mixed.txt", b"foo,Bad Tag")
    kept = run(home, "tag", "add", "--tag", "z", "--", mixed)
    assert kept.returncode == 0 and "tag clear" in kept.stderr, kept.stderr
    assert os.getxattr(mixed, "user.omafiles.tags") == b"foo,Bad Tag"
    assert row(mixed) == ["foo", "z"]
    last = carrying("last.txt", b"foo,Bad Tag")
    refused = run(home, "tag", "rm", "--tag", "foo", "--", last)
    assert refused.returncode == 1 and "tag clear" in refused.stderr, refused.stderr
    assert os.getxattr(last, "user.omafiles.tags") == b"foo,Bad Tag"


def check_removing_no_value_is_not_a_refusal(data, workspace):
    """The kernel asks for permission before it looks, so a file nobody may
    write refuses the removal of a value it does not hold."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r2b-novalue")
    plain = w / "plain"
    plain.write_text("x")
    plain.chmod(0o444)
    cleared = run(home, "tag", "clear", "--", plain)
    assert cleared.returncode == 0, (cleared.returncode, cleared.stderr)
    assert cleared.stderr == "", cleared.stderr


def check_tag_merge_says_the_mirror_it_skipped(data, workspace):
    """A mirror `tag merge` did not write is said, as `tag add` says one."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r2b-merge")
    (w / "shut").mkdir()
    (w / "shut" / "in.txt").write_text("i")
    assert run(home, "tag", "add", "--tag", "mm", "--", w / "shut" / "in.txt").returncode == 0
    (w / "shut").chmod(0o000)
    try:
        merged = run(home, "tag", "merge", "mm", "nn")
        assert merged.returncode == 0, merged.stderr
        assert "in.txt" in merged.stderr and "not written" in merged.stderr, merged.stderr
        quiet = run(home, "--quiet", "tag", "merge", "nn", "oo")
        assert quiet.returncode == 0 and quiet.stderr == "", quiet.stderr
    finally:
        (w / "shut").chmod(0o755)


def check_a_flag_takes_one_value_once(data, workspace):
    """A flag holding one value refuses a second, and `--flag=value` is the
    next word spelled attached."""
    home, w = fresh(workspace, "r2b-flags")
    for name in ("a", "b", "c"):
        (w / name).write_text(name)
    assert run(home, "tag", "add", "--tag", "x", "--", w / "a").returncode == 0
    twice = run(home, "tag", "cp", "--from", w / "a", "--from", w / "b", "--", w / "c")
    assert twice.returncode == 1 and "--from was given twice" in twice.stderr, twice.stderr
    assert run(home, "json", "--", w / "c").stdout == ""
    for words in (("ls", "--since", "1d", "--since", "2d"),
                  ("ls", "--name", "a", "--name=b")):
        refused = run(home, *words)
        assert refused.returncode == 1 and "given twice" in refused.stderr, (words, refused.stderr)
    slash = run(home, "ls", "--name", "x/y")
    assert slash.returncode == 1 and "never match" in slash.stderr, slash.stderr

    attached = run(home, "ls", "--tag=x")
    assert attached.returncode == 0 and attached.stdout.splitlines() == [str(w / "a")], \
        (attached.stdout, attached.stderr)
    value = run(home, "ls", "--tag=x", "--name=-j")
    assert value.returncode == 0 and value.stdout == "", (value.stdout, value.stderr)
    empty = run(home, "ls", "--name=")
    assert empty.returncode == 1 and "needs a pattern" in empty.stderr, empty.stderr
    lined = run(home, "batch", stdin=f"tag add --tag=q -- {w / 'b'}\n")
    assert lined.returncode == 0, lined.stderr
    assert json.loads(run(home, "json", "--", w / "b").stdout)["tags"] == ["q"]
    offered = run(home, "__complete", "--", "ls", "--tag=")
    assert offered.stdout.splitlines() == ["--tag=q", "--tag=x"], offered.stdout


def check_completion_offers_saved_names(data, workspace):
    """After `view rm --name` and `roots rm --id`, the names there are."""
    home, w = fresh(workspace, "r2b-names")
    assert run(home, "view", "add", "--name", "my view", "--tag", "t").returncode == 0
    assert run(home, "roots", "add", "--id", "shelf", "--", w).returncode == 0
    views = run(home, "__complete", "--", "view", "rm", "--name", "")
    assert views.returncode == 0 and views.stdout.splitlines() == ["my view"], views.stdout
    ids = run(home, "__complete", "--", "roots", "rm", "--id", "")
    assert ids.returncode == 0 and ids.stdout.splitlines() == ["shelf"], ids.stdout


def check_a_batch_vets_every_line_first(data, workspace):
    """A batch refuses before its first line runs, keeps its writes to the
    index, and reads its file as bytes."""
    home, w = fresh(workspace, "r2b-batch")
    target = w / "t.txt"
    target.write_text("t")
    assert run(home, "view", "add", "--name", "v1", "--tag", "t").returncode == 0
    # Line 1 would fail if it ran; the refusal of line 2 comes first.
    for line in ("view rm --name v1", f"roots add --id r -- {w}", "sweep --status",
                 f"sweep {w}", "import -"):
        refused = run(home, "batch", stdin=f"tag add --tag early -- {w / 'nothere'}\n{line}\n")
        assert refused.returncode == 1 and "line 2" in refused.stderr, (line, refused.stderr)
        assert "path missing" not in refused.stderr, (line, refused.stderr)
    assert run(home, "view", "ls").stdout.startswith("v1\t")
    assert run(home, "--dry-run", "roots", "add", "--id", "dry", "--", w).returncode == 0
    assert run(home, "roots", "ls").stdout == ""

    # The refusal a line makes for itself is exit 1 in a dry run too.
    (w / "iv").mkdir()
    bad = w / "iv" / "g"
    bad.write_text("g")
    try:
        os.setxattr(bad, "user.omafiles.tags", b"Bad Tag!")
    except OSError:
        bad = None
    if bad is not None:
        dry = run(home, "--dry-run", "batch", stdin=f"reindex {w / 'iv'}\n")
        assert dry.returncode == 1, (dry.returncode, dry.stderr)

    # Bytes: a `\r` inside a word is a character, and a BOM at byte 0 is dropped.
    cr = w / "cr\r"
    cr.write_text("c")
    carried = run(home, "batch", stdin=f'tag add --tag cr -- "{cr}"\n')
    assert carried.returncode == 0, carried.stderr
    assert json.loads(run(home, "json", "--", cr).stdout)["tags"] == ["cr"]
    commands = w / "bom.batch"
    commands.write_bytes(b"\xef\xbb\xbf" + f"tag add --tag bom -- {target}\n".encode())
    marked = run(home, "batch", commands)
    assert marked.returncode == 0, marked.stderr
    assert json.loads(run(home, "json", "--", target).stdout)["tags"] == ["bom"]


def check_a_batch_writes_each_mirror_once(data, workspace):
    """Two lines over one path are one xattr write after the commit, and its
    refusal is said once, named by the line that queued it."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r2b-once")
    locked = w / "locked.txt"
    locked.write_text("l")
    locked.chmod(0o444)
    done = run(home, "batch", stdin=f"tag add --tag a -- {locked}\ntag add --tag b -- {locked}\n")
    assert done.returncode == 0, done.stderr
    assert done.stderr.count("xattr set failed") == 1, done.stderr
    assert done.stderr.startswith("tag add: xattr set failed"), done.stderr
    alone = run(home, "tag", "add", "--tag", "c", "--", locked)
    assert alone.stderr.startswith("tag add: xattr set failed"), alone.stderr


def check_sweep_answers_whole(data, workspace):
    """sweep steps aside for another writer only, sweeps the roots it can,
    answers with every key, and keeps a receipt it can read back."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r2b-sweep")
    (w / "p" / "r").mkdir(parents=True)
    (w / "h").mkdir()
    carried = w / "h" / "f.txt"
    carried.write_text("f")
    try:
        os.setxattr(carried, "user.omafiles.tags", b"hx")
    except OSError:
        return
    assert run(home, "roots", "add", "--id", "pr", "--", w / "p" / "r").returncode == 0
    assert run(home, "roots", "add", "--id", "h", "--", w / "h").returncode == 0
    never = json.loads(run(home, "--json", "sweep", "--status").stdout)
    assert never["at"] is None and never["unlistable"] == 0, never
    (w / "p").chmod(0o000)
    try:
        swept = run(home, "--json", "sweep")
    finally:
        (w / "p").chmod(0o755)
    assert swept.returncode == 0, (swept.returncode, swept.stderr)
    answer = json.loads(swept.stdout)
    assert answer["adopted"] == [str(carried)], answer
    assert answer["unreadable"] == [str(w / "p" / "r")], answer
    receipt = json.loads(run(home, "--json", "sweep", "--status").stdout)
    assert receipt["adopted"] == 1 and receipt["unlistable"] == 0, receipt
    assert "unlistable\t0" in run(home, "sweep", "--status").stdout

    state = home / "state" / "omafiles" / "sweep.json"
    for forged in ({**receipt, "at": "x"}, {**receipt, "adopted": -3},
                   {**receipt, "trees": [1]}):
        state.write_text(json.dumps(forged))
        told = run(home, "sweep", "--status")
        assert told.returncode == 1 and "is not a receipt" in told.stderr, (forged, told.stderr)
    state.unlink()
    (home / "state" / "omafiles").chmod(0o555)
    try:
        noted = run(home, "sweep")
    finally:
        (home / "state" / "omafiles").chmod(0o755)
    assert noted.returncode == 0 and "cannot write the receipt" in noted.stderr, noted.stderr

    # A named tree under a root that is away is a leftover, not the root.
    disk = w / "disk"
    disk.mkdir()
    (disk / ".here").write_text("")
    (disk / "left.txt").write_text("l")
    assert run(home, "roots", "add", "--id", "disk", "--check", ".here", "--", disk).returncode == 0
    (disk / ".here").unlink()
    leftover = run(home, "sweep", disk)
    assert leftover.returncode == 1 and "declared and not here" in leftover.stderr, leftover.stderr
    assert not (home / "cache" / "omafiles" / "names" / "disk").exists()

    # Nothing here to sweep is still every key.
    bare, _ = fresh(workspace, "r2b-sweep-bare")
    lone = w / "lone"
    lone.mkdir()
    (lone / ".here").write_text("")
    assert run(bare, "roots", "add", "--id", "lone", "--check", ".here", "--", lone).returncode == 0
    (lone / ".here").unlink()
    nothing = json.loads(run(bare, "--json", "sweep").stdout)
    assert set(nothing) == {"trees", "adopted", "merged", "gone", "unreadable", "skipped",
                            "listed", "unlistable"}, nothing

    # An index that will not read is not another writer.
    index = home / "omafiles" / "index.jsonl"
    index.write_text(index.read_text() + "broken\n")
    broken = run(home, "sweep", w / "h")
    assert broken.returncode == 3 and "malformed jsonl" in broken.stderr, broken.stderr
    assert "stepped aside" not in broken.stderr, broken.stderr


def check_the_store_says_why(data, workspace):
    """A lock says who holds it or why the system refused it, and two lines
    for one path are a malformed index."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r2b-store")
    (w / "a").write_text("a")
    assert run(home, "tag", "add", "--tag", "x", "--", w / "a").returncode == 0
    (home / "omafiles").chmod(0o555)
    try:
        refused = run(home, "tag", "add", "--tag", "y", "--", w / "a")
    finally:
        (home / "omafiles").chmod(0o755)
    assert refused.returncode == 3 and "cannot lock" in refused.stderr, refused.stderr
    assert "Permission denied" in refused.stderr, refused.stderr

    lock = forge_lock(home)
    try:
        held = run(home, "sweep", w, extra_env={})
        waited = run(home, "tag", "add", "--tag", "y", "--", w / "a")
    finally:
        lock.unlink(missing_ok=True)
    assert held.returncode == 0 and "stepped aside" in held.stderr, held.stderr
    assert waited.returncode == 3 and "another writer holds it" in waited.stderr, waited.stderr

    index = home / "omafiles" / "index.jsonl"
    index.write_text(index.read_text() + json.dumps({"path": str(w / "a"), "tags": ["z"]}) + "\n")
    twin = run(home, "tags")
    assert twin.returncode == 3, (twin.returncode, twin.stderr)
    assert f"index.jsonl:2: {w / 'a'} is on an earlier line too" in twin.stderr, twin.stderr


def check_a_root_is_declared_and_undeclared_truly(data, workspace):
    """`--check` is inside the root and there now; a dry run declares
    nothing; `roots rm` says where its rows landed."""
    home, w = fresh(workspace, "r2b-roots")
    outer = w / "outer"
    inner = outer / "inner"
    inner.mkdir(parents=True)
    (inner / "x.txt").write_text("x")
    (outer / ".here").write_text("")
    for check, why in (("/etc", "relative to it"), ("../w", "relative to it"),
                       ("./.here", "relative to it"), ("nothere", "is not there")):
        refused = run(home, "roots", "add", "--id", "o", "--check", check, "--", outer)
        assert refused.returncode == 1 and why in refused.stderr, (check, refused.stderr)
    assert run(home, "roots", "ls").stdout == ""
    assert run(home, "roots", "add", "--id", "o", "--check", ".here", "--", outer).returncode == 0
    assert run(home, "roots", "add", "--id", "i", "--", inner).returncode == 0
    assert run(home, "tag", "add", "--tag", "t", "--", inner / "x.txt").returncode == 0
    dry = run(home, "--dry-run", "roots", "rm", "--id", "i")
    assert dry.stdout == "1 row(s) would be written back under root o\n", dry.stdout
    gone = run(home, "roots", "rm", "--id", "i")
    assert gone.returncode == 0, gone.stderr
    assert gone.stdout == "1 row(s) written back under root o\n", gone.stdout
    exported = json.loads(run(home, "export").stdout)
    assert (exported["root"], exported["path"]) == ("o", "inner/x.txt"), exported


def check_an_input_says_why_it_will_not_read(data, workspace):
    """A file that will not read or write says what the system said, and a
    command offered for pasting pastes."""
    import shlex
    home, w = fresh(workspace, "r2b-input")
    folder = run(home, "batch", w)
    assert folder.returncode == 1 and "Is a directory" in folder.stderr, folder.stderr
    absent = run(home, "batch", w / "nothere")
    assert absent.returncode == 2 and "No such file" in absent.stderr, absent.stderr
    if os.geteuid() != 0:
        (home / "config" / "omafiles").mkdir(parents=True)
        (home / "config" / "omafiles").chmod(0o555)
        try:
            saved = run(home, "view", "add", "--name", "v", "--tag", "t")
        finally:
            (home / "config" / "omafiles").chmod(0o755)
        assert saved.returncode == 3 and "Permission denied" in saved.stderr, saved.stderr
    odd = w / "it's"
    odd.write_text("o")
    try:
        os.setxattr(odd, "user.omafiles.tags", b"Bad Tag!")
    except OSError:
        return
    found = run(home, "verify", w)
    detail = [line for line in found.stdout.splitlines() if line.startswith("xattr-invalid")]
    assert len(detail) == 1, found.stdout
    command = detail[0].split("`")[1]
    assert shlex.split(command) == ["omafiles", "tag", "clear", "--", str(odd)], command


def in_dir(directory):
    """A `shell` for run_bytes that runs the binary from `directory`, which
    `run_bytes` hands over as `$W`."""
    return b'cd "$W" && exec "$0" "$@"', {b"W": os.fsencode(directory)}


def check_every_listing_reads_a_row_under_a_file_as_gone(data, workspace):
    """A row whose directory became a file is gone to `ls` and `tags`, as it
    is to `prune`, `verify` and `sweep`."""
    home = workspace / "listing-under-a-file"
    folder = home / "p"
    folder.mkdir(parents=True)
    (folder / "f").write_text("f")
    assert run_bytes(home, "tag", "add", "--tag", "zz", "--", folder / "f").returncode == 0
    shutil.rmtree(folder)
    folder.write_text("a file where the folder was")
    for words in (("ls", "--tag", "zz"), ("tags",)):
        done = run_bytes(home, *words)
        assert done.returncode == 0, (words, done.returncode, done.stderr)
        assert done.stdout == b"" and done.stderr == b"", (words, done.stdout, done.stderr)


def check_an_away_root_wins_over_every_operand_and_walk(data, workspace):
    """A root that is away wins over the disk for an operand, relative or not,
    and for a walk: a stray file in the mount point is not the storage."""
    home = workspace / "away-operands"
    disk = home / "RT" / "disk"
    disk.mkdir(parents=True)
    for name in (".here", "f", "g", "h"):
        (disk / name).write_text(name)
    assert run_bytes(home, "roots", "add", "--id", "disk", "--check", ".here",
                     "--", disk).returncode == 0
    assert run_bytes(home, "tag", "add", "--tag", "t", "--", disk / "f",
                     disk / "g").returncode == 0
    (disk / ".here").unlink()
    (disk / "h").write_bytes(b"h")
    os.setxattr(disk / "h", "user.omafiles.tags", b"stray")
    before = run_bytes(home, "export").stdout
    away = b"root disk is declared and not here"
    shell, env = in_dir(home)
    relative = run_bytes(home, "tag", "add", "--tag", "z", "--", "RT/disk/nonexist",
                         shell=shell, extra_env=env)
    assert relative.returncode == 1 and away in relative.stderr, relative
    for words in (("--yes", "rm", "--"), ("has", "--tag", "t", "--"), ("json", "--"),
                  ("tag", "ls", "--"), ("--dry-run", "mv", "--"), ("chmod", "600", "--")):
        operands = (disk / "g", home / "elsewhere") if "mv" in words else (disk / "g",)
        done = run_bytes(home, *words, *operands)
        assert done.returncode == 1, (words, done.returncode, done.stdout, done.stderr)
        assert away in done.stderr and done.stdout == b"", (words, done.stdout, done.stderr)
    assert (disk / "g").read_text() == "g"
    for words in (("ls", "--untagged"), ("ls", "--name", "*"), ("reindex",)):
        done = run_bytes(home, *words, disk)
        assert done.returncode == 1, (words, done.returncode, done.stdout, done.stderr)
        assert away in done.stderr and done.stdout == b"", (words, done.stdout, done.stderr)
    checked = run_bytes(home, "verify", disk)
    assert b"unindexed" not in checked.stdout and checked.returncode == 1, checked
    assert away in checked.stderr, checked.stderr
    # A link from outside the root into it leads to the same stray.
    (home / "RT" / "lnk").symlink_to(disk / "h")
    rebuilt = run_bytes(home, "reindex", home / "RT")
    assert rebuilt.returncode == 1 and away in rebuilt.stderr, rebuilt
    assert b"unindexed" not in run_bytes(home, "verify", home / "RT").stdout
    assert run_bytes(home, "export").stdout == before


def check_the_current_directory_is_an_input(data, workspace):
    """A current directory that was deleted, or is not UTF-8, runs nothing:
    Qt spells the one "" and the other with U+FFFD."""
    home = workspace / "cwd-input"
    (home / "u").mkdir(parents=True)
    kept = home / "u" / "b"
    kept.write_text("b")
    gone = home / "u" / "a"
    gone.write_text("a")
    assert run_bytes(home, "tag", "add", "--tag", "x", "--", gone, kept).returncode == 0
    gone.unlink()
    deleted = home / "t"
    deleted.mkdir()
    before = run_bytes(home, "export").stdout
    shell = b'cd "$W" && rmdir "$W" && exec "$0" "$@"'
    for words in (("--yes", "prune"), ("verify",), ("ls", "--untagged"), ("reindex",),
                  ("ls", "--tag", "x", str(home))):
        deleted.mkdir(exist_ok=True)
        done = run_bytes(home, *words, shell=shell, extra_env={b"W": os.fsencode(deleted)})
        assert done.returncode == 1, (words, done.returncode, done.stdout, done.stderr)
        assert b"current directory" in done.stderr and os.strerror(2).encode() in done.stderr, \
            (words, done.stderr)
        assert done.stdout == b"", (words, done.stdout)
    deleted.mkdir(exist_ok=True)
    quiet = run_bytes(home, "__complete", "--", "", shell=shell,
                      extra_env={b"W": os.fsencode(deleted)})
    assert quiet.returncode == 1 and quiet.stdout == b"" and quiet.stderr == b"", quiet
    assert run_bytes(home, "export").stdout == before

    spelled = os.fsencode(home) + b"/d\xfe"
    os.mkdir(spelled)
    with open(spelled + b"/a", "w") as handle:
        handle.write("a")
    done = run_bytes(home, "tag", "add", "--tag", "t", "--", "a",
                     shell=b'cd "$W" && exec "$0" "$@"', extra_env={b"W": spelled})
    assert done.returncode == 1, done
    assert b"current directory" in done.stderr and b"\\xfe" in done.stderr, done.stderr
    assert run_bytes(home, "export").stdout == before


def check_a_destination_resolves_as_the_kernel_does(data, workspace):
    """`L/..` after a symlink is the parent of where it leads, POSIX 4.13, for
    an `mv` or `cp` destination and a link operand alike; a destination in `/`
    is `/name`."""
    home = workspace / "kernel-resolution"
    (home / "x" / "y").mkdir(parents=True)
    (home / "L").symlink_to("x/y")
    for name in ("c", "f", "f2"):
        (home / "x" / name).write_text(name)
    (home / "x" / "lnk").symlink_to("c")
    shell, env = in_dir(home)
    for verb in ("mv", "cp"):
        done = run_bytes(home, "--dry-run", verb, "L/../c", "L/../c2", shell=shell, extra_env=env)
        assert done.returncode == 0, (verb, done.stderr)
        assert done.stdout == os.fsencode(home) + b"/x/c2\n", (verb, done.stdout)
        taken = run_bytes(home, "--dry-run", verb, "L/../f", "L/../f2", shell=shell,
                          extra_env=env)
        assert taken.returncode == 1 and b"already exists" in taken.stderr, (verb, taken)
    named = run_bytes(home, "--dry-run", "rm", "L/../lnk", shell=shell, extra_env=env)
    assert named.returncode == 0, named.stderr
    assert named.stdout == os.fsencode(home) + b"/x/lnk\n", named.stdout
    (home / "mm").write_text("m")
    assert run_bytes(home, "tag", "add", "--tag", "m", "--", home / "mm").returncode == 0
    into_root = run_bytes(home, "--json", "--dry-run", "mv", home / "mm", "/")
    assert json.loads(into_root.stdout)["to"] == "/mm", into_root.stdout
    assert json.loads(into_root.stdout)["rows"][0]["path"] == "/mm", into_root.stdout


def check_a_row_moved_out_of_a_root_is_written_where_it_is(data, workspace):
    """A row's stored form follows its path out of a root."""
    home = workspace / "moved-out-of-root"
    root = home / "R"
    (root / "d").mkdir(parents=True)
    (root / "a").write_text("a")
    (root / "d" / "x").write_text("x")
    assert run_bytes(home, "roots", "add", "--id", "r1", "--", root).returncode == 0
    assert run_bytes(home, "tag", "add", "--tag", "t", "--", root / "a",
                     root / "d" / "x").returncode == 0
    assert run_bytes(home, "mv", root / "d", home / "outd").returncode == 0
    assert run_bytes(home, "mv", root / "a", home / "a").returncode == 0
    rows = [json.loads(line) for line in run_bytes(home, "export").stdout.splitlines()]
    assert sorted(row["path"] for row in rows) == [str(home / "a"), str(home / "outd" / "x")], rows
    assert all("root" not in row for row in rows), rows
    assert run_bytes(home, "verify", home).returncode == 0


def check_the_index_holds_only_the_shape_it_writes(data, workspace):
    """What is read back is held to the shape this program writes: index
    rows, dump rows, roots declarations and lists of names."""
    home = workspace / "only-its-shape"
    home.mkdir()
    subject = home / "c.txt"
    subject.write_text("c")
    index = home / "data" / "omafiles" / "index.jsonl"
    index.parent.mkdir(parents=True)
    for row in ({"path": str(subject), "tags": ["Bad Tag"]},
                {"path": str(subject), "tags": ["UPPER"]},
                {"path": str(subject), "tags": [""]},
                {"path": "rel/c.txt", "tags": ["x"]},
                {"path": f"/{home}//c.txt/", "tags": ["x"]}):
        index.write_text(json.dumps(row) + "\n")
        done = run_bytes(home, "tags")
        assert done.returncode == 3, (row, done.returncode, done.stderr)
        assert b"store: malformed jsonl at" in done.stderr, done.stderr
    index.unlink()

    for line in (b'{"path":"' + os.fsencode(subject) + b'\\u0000evil","tags":["nul"]}',
                 b'{"path":"' + os.fsencode(home) + b'/\\ud800","tags":["lone"]}',
                 b'{"path":"' + os.fsencode(home) + b'//t/b/","tags":["s"]}',
                 b'{"path":"rel","tags":["s"]}'):
        done = run_bytes(home, "import", "-", stdin=line + b"\n")
        assert done.returncode == 1, (line, done.stderr)
        assert b"\x00" not in done.stderr, done.stderr
    assert b"user.omafiles.tags" not in b"".join(n.encode() for n in os.listxattr(subject))
    (home / "r").mkdir()
    assert run_bytes(home, "roots", "add", "--id", "r", "--", home / "r").returncode == 0
    escaped = run_bytes(home, "import", "-",
                        stdin=b'{"root":"r","path":"../outside","tags":["esc"]}\n')
    assert escaped.returncode == 1 and b"inside root r" in escaped.stderr, escaped.stderr
    assert run_bytes(home, "export").stdout == b""

    names = home / "cache" / "omafiles" / "names" / "r"
    names.parent.mkdir(parents=True)
    names.write_bytes(b"ok\n../../etc/passwd\n")
    cached = run_bytes(home, "ls", "--untagged", "--cached")
    assert cached.returncode == 1 and b"is not a clean path" in cached.stderr, cached.stderr
    assert b"passwd" not in cached.stdout, cached.stdout

    roots_file = home / "config" / "omafiles" / "roots.jsonl"
    escape = home / "escaped"
    roots_file.write_text(json.dumps({"id": f"../../../{escape.name}", "path": str(home / "r")})
                          + "\n")
    swept = run_bytes(home, "sweep")
    assert swept.returncode == 3 and b"is not an id" in swept.stderr, swept.stderr
    assert not escape.exists()
    for root in ({"id": "r", "path": "r"}, {"id": "r", "path": str(home / "r"), "check": "../m"}):
        roots_file.write_text(json.dumps(root) + "\n")
        refused = run_bytes(home, "roots")
        assert refused.returncode == 3 and str(roots_file).encode() in refused.stderr, refused


def check_a_name_may_begin_with_a_byte_order_mark(data, workspace):
    """U+FEFF is a character, and valid UTF-8 that begins with it is a name.
    A dump that begins with one is read as if it did not, Python's `utf-8-sig`."""
    home = workspace / "byte-order-mark"
    home.mkdir()
    marked = os.fsencode(home) + b"/\xef\xbb\xbfx"
    with open(marked, "w") as handle:
        handle.write("b")
    done = run_bytes(home, "tag", "add", "--tag", "b", "--", marked)
    assert done.returncode == 0, done.stderr
    walked = run_bytes(home, "ls", "--name", "*x", home)
    assert walked.returncode == 0 and walked.stdout == marked + b"\n", walked
    plain = home / "p"
    plain.write_text("p")
    dump = b"\xef\xbb\xbf" + json.dumps({"path": str(plain), "tags": ["bom"]}).encode() + b"\n"
    for shape in (dump, b"\xef\xbb\xbf[" + dump[3:].strip() + b"]"):
        imported = run_bytes(home, "import", "-", stdin=shape)
        assert imported.returncode == 0, (shape, imported.stderr)
    assert run_bytes(home, "tag", "ls", "--", plain).stdout == b"bom\n"


def check_a_shut_directory_says_what_the_system_said(data, workspace):
    """A directory that would not open is said with the system's reason."""
    home = workspace / "shut-says-why"
    shut = home / "shut"
    shut.mkdir(parents=True)
    (shut / "f").write_text("f")
    assert run_bytes(home, "tag", "add", "--tag", "k", "--", shut / "f").returncode == 0
    denied = os.strerror(13).encode()
    try:
        shut.chmod(0o000)
        for words, done in ((("ls", "--untagged", home), b"listed"),
                            (("ls", "--tag", "k", shut), b"listed"),
                            (("tags", shut), b"counted"), (("--yes", "prune", shut), b"dropped"),
                            (("reindex", home), b"reindexed")):
            said = run_bytes(home, *words)
            assert said.returncode == 1, (words, said.returncode, said.stderr)
            assert os.fsencode(shut) + b" is unreadable; nothing under it was " + done + b": " \
                + denied in said.stderr, (words, said.stderr)
        checked = run_bytes(home, "verify", home)
        assert b"\tthe tree under it was not walked: " + denied in checked.stdout, checked.stdout
    finally:
        shut.chmod(0o755)

    # And a declaration or a list of names that will not write says why.
    assert run_bytes(home, "roots", "add", "--id", "h", "--", home).returncode == 0
    assert run_bytes(home, "sweep").returncode == 0
    for kept, words in ((home / "config" / "omafiles", ("roots", "add", "--id", "s", "--", shut)),
                        (home / "cache" / "omafiles" / "names", ("sweep",))):
        try:
            kept.chmod(0o555)
            said = run_bytes(home, *words)
        finally:
            kept.chmod(0o755)
        assert b"cannot write" in said.stderr and denied in said.stderr, (words, said.stderr)


def check_import_says_a_mirror_it_did_not_attempt(data, workspace):
    """A row `import` could not stamp onto its file is said, as `tag add` says.

    A path from another machine is silent: that is the point of a dump. One
    the system will not describe is not the same thing."""
    home = workspace / "import-mirror-note"
    shut = home / "M" / "shut"
    shut.mkdir(parents=True)
    (shut / "in").write_text("i")
    dump = "".join(json.dumps({"path": path, "tags": ["mm"]}) + "\n"
                   for path in (str(shut / "in"), "/nowhere/at/all"))
    try:
        shut.chmod(0o000)
        done = run_bytes(home, "import", "-", stdin=dump.encode())
        quiet = run_bytes(home, "--quiet", "import", "-", stdin=dump.encode())
    finally:
        shut.chmod(0o755)
    assert done.returncode == 0, done.stderr
    assert os.fsencode(shut / "in") in done.stderr and b"not written" in done.stderr, done.stderr
    assert b"nowhere" not in done.stderr, done.stderr
    assert quiet.returncode == 0 and quiet.stderr == b"", quiet.stderr


def check_a_copy_carries_bytes_mode_and_tags(data, workspace):
    """A copy of a read-only file still carries its tags. `mv` across a
    filesystem keeps the whole mode; `cp` gives what coreutils gives without
    `-p`."""
    home = workspace / "copy-carries"
    home.mkdir()
    source = home / "a"
    source.write_text("a")
    assert run_bytes(home, "tag", "add", "--tag", "t", "--", source).returncode == 0
    try:
        os.getxattr(source, "user.omafiles.tags")
    except OSError:
        return                                   # no user.* here
    source.chmod(0o4755)
    copied = run_bytes(home, "cp", source, home / "c")
    assert copied.returncode == 0 and copied.stderr == b"", copied.stderr
    assert os.getxattr(home / "c", "user.omafiles.tags") == b"t"
    mask = os.umask(0)
    os.umask(mask)
    assert os.stat(home / "c").st_mode & 0o7777 == 0o755 & ~mask, oct(os.stat(home / "c").st_mode)
    source.chmod(0o444)
    readonly = run_bytes(home, "cp", source, home / "r")
    assert readonly.returncode == 0 and readonly.stderr == b"", readonly.stderr
    assert os.getxattr(home / "r", "user.omafiles.tags") == b"t"
    assert run_bytes(home, "verify", home).returncode == 0

    shm = Path("/dev/shm")
    if not shm.is_dir() or os.stat(shm).st_dev == os.stat(home).st_dev:
        return                                   # one filesystem; nothing to cross
    across = Path(tempfile.mkdtemp(prefix="omafiles-carries-", dir=str(shm)))
    try:
        try:
            os.setxattr(across, "user.omafiles.probe", b"x")
        except OSError:
            return                               # no user.* at the far end
        for mode in (0o444, 0o4755):
            moving = home / f"m{mode:o}"
            moving.write_text("m")
            assert run_bytes(home, "tag", "add", "--tag", "m", "--", moving).returncode == 0
            moving.chmod(mode)
            moved = run_bytes(home, "mv", moving, across / moving.name)
            assert moved.returncode == 0 and moved.stderr == b"", (mode, moved.stderr)
            assert os.getxattr(across / moving.name, "user.omafiles.tags") == b"m"
            assert os.stat(across / moving.name).st_mode & 0o7777 == mode, mode
        assert run_bytes(home, "verify", across).returncode == 0
    finally:
        shutil.rmtree(across, ignore_errors=True)


def check_cp_takes_regular_files_only(data, workspace):
    """`cp` from a fifo is refused, dry run or not, rather than waiting on it."""
    home = workspace / "cp-regular"
    home.mkdir()
    os.mkfifo(home / "ff")
    for words in (("--dry-run", "cp"), ("cp",)):
        try:
            done = subprocess.run([str(CLI), *words, str(home / "ff"), str(home / "g")],
                                  env=cli_env(home), cwd=str(ROOT), capture_output=True,
                                  stdin=subprocess.DEVNULL, timeout=10)
        except subprocess.TimeoutExpired:
            raise AssertionError(f"{words} waited on a fifo")
        assert done.returncode == 1 and b"not a regular file" in done.stderr, done
    assert not (home / "g").exists()


def check_rm_and_trash_refuse_the_root_directory(data, workspace):
    """`/` is refused by `rm` and `trash`, the way coreutils --preserve-root does."""
    home = workspace / "preserve-root"
    home.mkdir()
    for verb in ("rm", "trash"):
        done = run_bytes(home, "--dry-run", verb, "/")
        assert done.returncode == 1, (verb, done.returncode, done.stdout)
        assert b"/ is the whole filesystem" in done.stderr and done.stdout == b"", done


def check_completion_goes_on_past_a_broken_roots_file(data, workspace):
    """A roots file that will not parse costs `__complete` only the candidates
    read out of it: the verbs and the flags are still offered, and a root id
    is not. `completions` prints a script no declaration changes."""
    home = workspace / "complete-broken-roots"
    config = home / "config" / "omafiles"
    config.mkdir(parents=True)
    (config / "roots.jsonl").write_text("broken\n")
    verbs = run_bytes(home, "__complete", "--", "")
    assert verbs.returncode == 0 and b"tag\n" in verbs.stdout and verbs.stderr == b"", verbs
    ids = run_bytes(home, "__complete", "--", "roots", "rm", "--id", "")
    assert ids.returncode == 1 and ids.stdout == b"" and ids.stderr == b"", ids
    script = run_bytes(home, "completions", "bash")
    assert script.returncode == 0 and b"omafiles" in script.stdout, script
    assert run_bytes(home, "ls").returncode == 3


def check_a_relative_xdg_home_is_ignored(data, workspace):
    """A relative `$XDG_*_HOME` is ignored, as the XDG Base Directory spec says."""
    home = workspace / "relative-xdg"
    home.mkdir()
    (home / "a").write_text("a")
    shell, env = in_dir(home)
    done = run_bytes(home, "tag", "add", "--tag", "t", "--", "a", shell=shell,
                     extra_env={**env, b"XDG_DATA_HOME": b"rel"})
    assert done.returncode == 0, done.stderr
    assert not (home / "rel").exists()
    assert (home / "home" / ".local" / "share" / "omafiles" / "index.jsonl").is_file()


def check_a_command_offered_to_paste_is_quoted(data, workspace):
    """A command `verify` and `reindex` offer is quoted for a shell."""
    home = workspace / "paste-quoted"
    home.mkdir()
    odd = home / "a b'c"
    odd.write_text("o")
    try:
        os.setxattr(odd, "user.omafiles.tags", b"Bad Tag!")
    except OSError:
        return                                   # no user.* here
    quoted = f"`omafiles tag clear -- '{home}/a b'\"'\"'c'`"
    checked = run_bytes(home, "--json", "verify", home)
    assert quoted in json.loads(checked.stdout)["problems"][0]["detail"], checked.stdout
    assert quoted.encode() in run_bytes(home, "verify", home).stdout
    assert quoted.encode() in run_bytes(home, "reindex", home).stderr


def check_a_path_the_kernel_resolves_is_text_only_when_it_spells(data, workspace):
    """A symlink leading to a name that is not UTF-8 is refused by every verb
    that resolves it, and named by every walk that meets it: the name would be
    spelled with U+FFFD, which is some other file."""
    home = workspace / "r3-resolved-bytes"
    w = home / "w"
    w.mkdir(parents=True)
    bad = os.fsencode(w) + b"/bad\xff"
    with open(bad, "w") as out:
        out.write("b")
    (w / "bad�").write_text("the other file")      # what U+FFFD would name
    os.symlink(b"bad\xff", os.fsencode(w / "good"))
    os.mkdir(os.fsencode(w) + b"/bd\xfe")
    os.symlink(b"bd\xfe", os.fsencode(w / "gd"))
    for words in (("json", "--"), ("has", "--tag", "t", "--"), ("tag", "add", "-t", "t", "--"),
                  ("ls",)):
        target = w / ("gd" if words == ("ls",) else "good")
        done = run_bytes(home, *words, target)
        assert done.returncode == 1, (words, done.returncode, done.stdout, done.stderr)
        assert b"bad\\xff" in done.stderr or b"bd\\xfe" in done.stderr, (words, done.stderr)
        assert "�".encode() not in done.stdout + done.stderr, (words, done.stdout)
    assert not (home / "data" / "omafiles" / "index.jsonl").exists()
    tree = w / "t"
    tree.mkdir()
    os.symlink(b"../bad\xff", os.fsencode(tree / "l"))
    link = str(tree / "l").encode()
    rebuilt = run_bytes(home, "reindex", tree)
    assert rebuilt.returncode == 1 and link in rebuilt.stderr, rebuilt
    assert b"not reindexed" in rebuilt.stderr and b"No such file" not in rebuilt.stderr, rebuilt
    checked = run_bytes(home, "verify", tree)
    assert checked.returncode == 1, checked
    assert checked.stdout.startswith(b"unreadable\t" + link + b"\t"), checked.stdout
    swept = run_bytes(home, "--json", "sweep", tree)
    assert swept.returncode == 0 and json.loads(swept.stdout)["unreadable"] == [link.decode()], \
        swept


def check_a_path_realpath_may_not_walk_is_refused(data, workspace):
    """`realpath` walks from `/` where a relative `stat` asks only the cwd, so
    a cwd under a shut parent refuses the path rather than storing it as typed."""
    if os.geteuid() == 0:
        return
    home = workspace / "r3-realpath-eacces"
    inner = home / "shut" / "in"
    inner.mkdir(parents=True)
    (inner / "x").write_text("x")
    # In first, then shut behind: the cwd itself stays searchable.
    shell = b'cd "$W" && chmod 000 .. && { "$0" "$@"; s=$?; chmod 755 ..; exit $s; }'
    env = {b"W": os.fsencode(inner)}
    tagged = run_bytes(home, "tag", "add", "-t", "q", "--", "x", shell=shell, extra_env=env)
    opened = run_bytes(home, "--dry-run", "open", "x", shell=shell, extra_env=env)
    assert tagged.returncode == 1 and b"x: Permission denied" in tagged.stderr, tagged
    assert not (home / "data" / "omafiles" / "index.jsonl").exists()
    assert opened.returncode == 1 and opened.stdout == b"", opened


def check_each_hop_is_named_where_the_kernel_looks(data, workspace):
    """A dangling link reached through a symlinked directory, or through
    `L/..`, names the target the kernel would have looked for."""
    home = workspace / "r3-hops"
    w = home / "w"
    (w / "d" / "e").mkdir(parents=True)
    os.symlink("../gone", w / "d" / "e" / "dl")
    os.symlink("d/e", w / "le")
    os.symlink("d/e", w / "lz")
    os.symlink("lz/../nope", w / "y")
    (w / "gone").write_text("not the one")
    (w / "nope").write_text("not the one")
    through = run(home, "tag", "add", "-t", "a", w / "le" / "dl")
    assert through.returncode == 2, through.stderr
    assert f"is a symlink to {w}/d/gone, which is not there" in through.stderr, through.stderr
    dotdot = run(home, "tag", "add", "-t", "a", w / "y")
    assert f"is a symlink to {w}/d/nope, which is not there" in dotdot.stderr, dotdot.stderr


def check_a_trailing_slash_names_a_directory(data, workspace):
    """`mv a new/` and `cp b new/` refuse with ENOTDIR, as `rename(2)` and
    coreutils do; a directory may still be moved to a name ending in `/`."""
    home = workspace / "r3-trailing-slash"
    w = home / "w"
    w.mkdir(parents=True)
    (w / "a").write_text("a")
    (w / "d").mkdir()
    for verb in ("mv", "cp"):
        done = run(home, verb, w / "a", f"{w}/new/")
        assert done.returncode == 1 and "Not a directory" in done.stderr, (verb, done.stderr)
        assert not (w / "new").exists(), verb
    moved = run(home, "mv", w / "d", f"{w}/dnew/")
    assert moved.returncode == 0 and (w / "dnew").is_dir(), moved.stderr


def check_an_away_root_refuses_every_operand_it_is_handed(data, workspace):
    """A `[dir]`, a view's folder, a new root and a file to read under a root
    that is away are refused, and `sweep` names the root it did not sweep."""
    home = workspace / "r3-away-dir"
    w = home / "w"
    (w / "r4" / "x").mkdir(parents=True)
    assert run(home, "roots", "add", "--id", "four", "--mount", w / "r4").returncode == 0
    (w / "r4" / "b.txt").write_text(f"tag add -t q -- {w}/r4\n")
    for words in (("ls",), ("ls", "--json"), ("tags",), ("--yes", "prune"), ("verify",),
                  ("view", "add", "--name", "d"), ("roots", "add", "--id", "nest"),
                  ("--dry-run", "batch"), ("--dry-run", "import")):
        operand = w / "r4" / ("b.txt" if words[-1] in ("batch", "import") else "x")
        done = run(home, *words, operand)
        assert done.returncode == 1, (words, done.returncode, done.stdout, done.stderr)
        assert "root four is declared and not here" in done.stderr, (words, done.stderr)
        assert done.stdout == "", (words, done.stdout)
    shell, env = in_dir(w / "r4")
    here = run_bytes(home, "--yes", "prune", shell=shell,
                     extra_env={**env, b"XDG_DATA_HOME": os.fsencode(home)})
    assert here.returncode == 1 and b"declared and not here" in here.stderr, here
    swept = run(home, "sweep")
    assert swept.returncode == 0, swept.stderr
    assert "sweep: root four is declared and not here; it was not swept" in swept.stderr, \
        swept.stderr


def check_a_nested_root_is_listed_once_and_only_while_here(data, workspace):
    """Each cached path counts under its own root's list: never twice, and not
    at all while that root is away."""
    home = workspace / "r3-nested-cached"
    w = home / "w"
    (w / "O" / "in" / "deep").mkdir(parents=True)
    for name in ("O/a", "O/in/b", "O/in/deep/c", "O/in/.m"):
        (w / name).write_text("x")
    assert run(home, "roots", "add", "--id", "outer", w / "O").returncode == 0
    assert run(home, "roots", "add", "--id", "inner", "--check", ".m", w / "O" / "in").returncode == 0
    assert run(home, "sweep").returncode == 0
    names = home / "cache" / "omafiles" / "names"
    assert (names / "outer").read_text().splitlines() == ["a", "in"]
    # A list written before holds the inner root's paths too; each still
    # counts under its own root's list alone.
    with open(names / "outer", "a") as older:
        older.write("in/b\n")
    walked = run(home, "ls", "--untagged", w / "O").stdout
    for scope in ((), (w / "O",)):
        cached = run(home, "ls", "--untagged", "--cached", *scope)
        assert cached.returncode == 0, (scope, cached.stderr)
        assert cached.stdout == walked, (scope, cached.stdout, walked)
    (w / "O" / "in" / ".m").unlink()
    for scope in ((), (w / "O",)):
        away = run(home, "ls", "--untagged", "--cached", *scope)
        assert away.returncode == 1, (scope, away.stdout)
        assert away.stdout.splitlines() == [str(w / "O" / "a"), str(w / "O" / "in")], \
            (scope, away.stdout)
        assert "root inner is declared and not here" in away.stderr, (scope, away.stderr)


def check_a_row_now_a_symlink_is_missing(data, workspace):
    """A row whose own path is now a symlink is missing: `verify` says so,
    `prune` drops it, and no write goes through it to the file it leads to."""
    home = workspace / "r3-row-link"
    w = home / "w"
    w.mkdir(parents=True)
    (w / "a").write_text("a")
    (w / "b").write_text("b")
    assert run(home, "tag", "add", "-t", "x", w / "a").returncode == 0
    assert run(home, "tag", "add", "-t", "other", w / "b").returncode == 0
    (w / "a").unlink()
    os.symlink("b", w / "a")
    assert run(home, "tag", "merge", "x", "y").returncode == 0
    assert '"tags":["other"]' in run(home, "json", w / "b").stdout
    checked = run(home, "verify", w)
    assert checked.stdout.splitlines() == [f"missing\t{w}/a"], checked.stdout
    pruned = run(home, "--yes", "prune", w)
    assert pruned.returncode == 0 and pruned.stdout.splitlines() == [f"{w}/a"], pruned


def check_input_that_will_not_read_is_refused(data, workspace):
    """`batch` and `import` refuse a stdin that is a directory or closed, and a
    kept file that is a fifo is refused at once rather than read forever."""
    home = workspace / "r3-unread-input"
    w = home / "w"
    w.mkdir(parents=True)
    for verb in (("batch",), ("import", "-")):
        for opener, said in ((lambda: os.open(w, os.O_RDONLY), "Is a directory"),
                             (None, "Bad file descriptor")):
            fd = opener() if opener else None
            try:
                done = subprocess.run(
                    [str(CLI), *verb], cwd=str(w), env=cli_env(home), text=True,
                    stdin=fd if fd is not None else subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    preexec_fn=None if fd is not None else (lambda: os.close(0)))
            finally:
                if fd is not None:
                    os.close(fd)
            assert done.returncode == 1, (verb, said, done.returncode, done.stderr)
            assert f"{verb[0]}: cannot read stdin: {said}" in done.stderr, (verb, done.stderr)
    fifo = home / "omafiles" / "index.jsonl"
    fifo.parent.mkdir()
    os.mkfifo(fifo)
    (w / "f").write_text("f")
    for words in (("ls",), ("--yes", "rm", w / "f")):
        done = subprocess.run(["timeout", "5", str(CLI), *map(str, words)], env=cli_env(home),
                              text=True, capture_output=True, stdin=subprocess.DEVNULL)
        assert done.returncode == 3, (words, done.returncode, done.stderr)
        assert "is not a regular file" in done.stderr, (words, done.stderr)
    assert (w / "f").exists()


def check_sweep_and_reindex_restamp_what_they_change(data, workspace):
    """`tagged` is when a row's tags last changed, `sweep` and `reindex
    --from-xattr` included; a row they leave as it was keeps its stamp."""
    home = workspace / "r3-restamp"
    w = home / "w"
    (w / "t").mkdir(parents=True)
    for name in ("a", "c", "k"):
        (w / "t" / name).write_text(name)
    try:
        os.setxattr(w / "t" / "a", "user.omafiles.probe", b"1")
    except OSError:
        return                                   # no user.* here
    past = 1_000_000_000
    write_index(home, [{"path": str(w / "t" / name), "tagged": past, "tags": ["one"]}
                       for name in ("a", "c", "k")])
    for name in ("a", "c", "k"):
        os.setxattr(w / "t" / name, "user.omafiles.tags", b"one")
    os.setxattr(w / "t" / "a", "user.omafiles.tags", b"one,two")
    assert run(home, "sweep", w / "t").returncode == 0
    os.setxattr(w / "t" / "c", "user.omafiles.tags", b"two")
    assert run(home, "reindex", "--from-xattr", w / "t").returncode == 0
    rows = {Path(row["path"]).name: row for row in
            map(json.loads, run(home, "export").stdout.splitlines())}
    assert rows["a"]["tags"] == ["one", "two"] and rows["a"]["tagged"] > past, rows
    assert rows["c"]["tags"] == ["two"] and rows["c"]["tagged"] > past, rows
    assert rows["k"]["tagged"] == past, rows


def check_view_and_roots_answer_as_every_writer_does(data, workspace):
    """`view rm --json` prints the receipt `view add` prints, `view ls` quotes
    the command it offers, and `view ls`/`roots ls` take no arguments."""
    import shlex
    home = workspace / "r3-view-roots"
    w = home / "w"
    (w / "d ir").mkdir(parents=True)
    assert run(home, "view", "add", "--name", "v", w / "d ir").returncode == 0
    listed = run(home, "view", "ls").stdout.strip().split("\t")
    assert shlex.split(listed[1]) == ["omafiles", "ls", str(w / "d ir")], listed
    for dry in (("--dry-run",), ()):
        removed = run(home, *dry, "--json", "view", "rm", "--name", "V")
        assert removed.returncode == 0 and json.loads(removed.stdout) == {"name": "v"}, \
            (dry, removed.stdout)
    for words in (("view", "ls", "x"), ("roots", "ls", "x")):
        done = run(home, *words)
        assert done.returncode == 1 and "takes no arguments" in done.stderr, (words, done)


def check_a_list_that_will_not_read_is_an_error(data, workspace):
    """A refused list of names is an error `--quiet` keeps; a scoped answer
    names only the gaps inside its `[dir]`; the sweep summary gives every
    count; `roots rm` says it moved nothing."""
    if os.geteuid() == 0:
        return
    home = workspace / "r3-list-errors"
    w = home / "w"
    (w / "r" / "sub").mkdir(parents=True)
    (w / "r" / "locked").mkdir()
    (w / "r" / "sub" / "a").write_text("a")
    assert run(home, "roots", "add", "--id", "r", w / "r").returncode == 0
    (w / "r" / "locked").chmod(0o000)
    try:
        swept = run(home, "sweep")
    finally:
        (w / "r" / "locked").chmod(0o755)
    assert re.search(r"sweep: adopted 0, merged 0, gone 0, unreadable 1, skipped 0, "
                     r"listed \d+, unlistable 0", swept.stderr), swept.stderr
    scoped = run(home, "ls", "--untagged", "--cached", w / "r" / "sub")
    assert scoped.returncode == 0 and "short" not in scoped.stderr, scoped
    whole = run(home, "ls", "--untagged", "--cached", w / "r")
    assert whole.returncode == 1 and "locked is unreadable" in whole.stderr, whole
    with open(home / "cache" / "omafiles" / "names" / "r", "a") as listing:
        listing.write("../x\n")
    refused = run(home, "--quiet", "ls", "--untagged", "--cached")
    assert refused.returncode == 1 and "is not a clean path" in refused.stderr, refused
    dropped = run(home, "roots", "rm", "--id", "r")
    assert dropped.returncode == 0 and dropped.stdout == "0 row(s) written back\n", dropped


def check_a_cache_it_cannot_make_says_why(data, workspace):
    """A list of names, a receipt or the roots whose directory will not be made
    names the system's reason for that directory, not the file's ENOENT after
    it."""
    if os.geteuid() == 0:
        return
    home = workspace / "r3-cache-reason"
    w = home / "w"
    (w / "r").mkdir(parents=True)
    assert run(home, "roots", "add", "--id", "r", w / "r").returncode == 0
    second = home / "second"
    for shut in (home / "cache", home / "state", second / "config"):
        shut.mkdir(parents=True, exist_ok=True)
        shut.chmod(0o500)
    try:
        swept = run(home, "sweep")
        declared = run(second, "roots", "add", "--id", "s", w)
    finally:
        for shut in (home / "cache", home / "state", second / "config"):
            shut.chmod(0o700)
    for said in (f"names: cannot write {home}/cache/omafiles/names/r: {home}/cache/omafiles",
                 f"cannot write the receipt {home}/state/omafiles/sweep.json: "
                 f"{home}/state/omafiles"):
        assert f"{said}: Permission denied" in swept.stderr, (said, swept.stderr)
    assert declared.returncode == 3 and f"roots: cannot write {second}/config/omafiles/" \
        f"roots.jsonl: {second}/config/omafiles: Permission denied" in declared.stderr, declared


def check_recent_reads_only_what_glib_reads(data, workspace):
    """An escaped `/` in an href is skipped and a document that is not xbel is
    refused, as GLib's bookmark parser does."""
    home = workspace / "r3-recent-glib"
    w = home / "w"
    (w / "d").mkdir(parents=True)
    (w / "d" / "a").write_text("a")
    xbel = home / "recently-used.xbel"
    xbel.write_text(f"""<?xml version="1.0"?><xbel version="1.0">
<bookmark href="file://{w}/d%2Fa" visited="2026-09-03T10:00:00Z"/></xbel>""")
    skipped = run(home, "recent", "--desktop")
    assert skipped.returncode == 0 and skipped.stdout == "", skipped
    xbel.write_text("<foo/>")
    refused = run(home, "recent", "--desktop")
    assert refused.returncode == 1 and "tag 'xbel' expected" in refused.stderr, refused


def check_a_relative_home_decides_no_directory(data, workspace):
    """`HOME` that is relative, or unset, where it decides a base directory is
    refused: the index would move with the cwd."""
    home = workspace / "r3-relative-home"
    home.mkdir()
    for value in (b"rel", b""):
        done = run_bytes(home, "ls", extra_env={b"HOME": value, b"XDG_DATA_HOME": b""})
        assert done.returncode == 1, (value, done)
        assert b"HOME is not an absolute path" in done.stderr, (value, done.stderr)
    assert run_bytes(home, "ls", extra_env={b"HOME": b"rel"}).returncode == 0


def check_a_check_must_stay_inside_its_root(data, workspace):
    """`roots add --check` through a link out of the root, or back to it, is
    refused; `--mount` on a drive unplugged now is accepted."""
    home = workspace / "r3-check-inside"
    w = home / "w"
    (w / "r").mkdir(parents=True)
    (w / "o").mkdir()
    (w / "o" / "hostname").write_text("h")
    os.symlink("../o", w / "r" / "lnk")
    os.symlink(".", w / "r" / "self")
    for check in ("lnk/hostname", "self"):
        done = run(home, "roots", "add", "--id", "r", "--check", check, w / "r")
        assert done.returncode == 1 and "which is not inside" in done.stderr, (check, done)
    assert run(home, "roots", "add", "--id", "m", "--mount", w / "o").returncode == 0


def check_no_system_path_becomes_text_outside_paths(data, workspace):
    """The calls that decode a path or a reason the system hands back lossily,
    or clean a path as text, live nowhere in the core, the CLI or the studio but
    `Paths.cpp`."""
    lossy = re.compile(r"canonicalFilePath|symLinkTarget|decodeName|entryList|entryInfoList"
                       r"|QDirIterator|toLocalFile|currentPath|homePath|QStandardPaths"
                       r"|qEnvironmentVariable|strerror|\.absolutePath\(|absoluteFilePath\(")
    found = []
    for source in sorted([*(ROOT / "src" / "core").glob("*.cpp"),
                          *(ROOT / "src" / "studio").glob("*.cpp"), ROOT / "src/cli/main.cpp"]):
        if source.name == "Paths.cpp":
            continue
        for number, line in enumerate(source.read_text().splitlines(), 1):
            if lossy.search(line) and not line.lstrip().startswith("//"):
                found.append(f"{source.relative_to(ROOT)}:{number}: {line.strip()}")
    assert not found, "\n".join(found)


def carries_xattrs(path):
    """Whether this filesystem takes a `user.*` value on `path`."""
    try:
        os.setxattr(path, "user.omafiles.probe", b"1")
        os.removexattr(path, "user.omafiles.probe")
        return True
    except OSError:
        return False


def check_a_value_nobody_may_read_is_not_no_value(data, workspace):
    """A file with no row whose value the system will not show is refused by
    every writer, exit 1, and its value survives: taking it for no value would
    let the write destroy it."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r3-unreadable-value")
    if not carries_xattrs(w):
        return
    shut = {}
    for name in ("rm.txt", "add.txt", "cp.txt", "clear.txt", "from.txt", "import.txt"):
        shut[name] = w / name
        shut[name].write_text(name)
        os.setxattr(shut[name], "user.omafiles.tags", b"foo,bar")
        shut[name].chmod(0o200)
    source = w / "open.txt"
    source.write_text("o")
    os.setxattr(source, "user.omafiles.tags", b"sx")
    (w / "dump").write_text(json.dumps({"path": str(shut["import.txt"]), "tags": ["z"]}) + "\n")
    try:
        for words in (("tag", "rm", "--tag", "foo", "--", shut["rm.txt"]),
                      ("tag", "add", "--tag", "z", "--", shut["add.txt"]),
                      ("tag", "cp", "--from", source, "--", shut["cp.txt"]),
                      ("tag", "cp", "--from", shut["from.txt"], "--", source),
                      ("tag", "clear", "--", shut["clear.txt"]),
                      ("import", w / "dump")):
            refused = run(home, *words)
            assert refused.returncode == 1, (words, refused.returncode, refused.stderr)
            assert "Permission denied" in refused.stderr, (words, refused.stderr)
            assert "has no tags" not in refused.stderr, (words, refused.stderr)
    finally:
        for path in shut.values():
            path.chmod(0o644)
    for path in shut.values():
        assert os.getxattr(path, "user.omafiles.tags") == b"foo,bar", path
    assert run(home, "export").stdout == "", "a refused write left a row"


def check_a_drop_with_no_row_is_reported(data, workspace):
    """`tag clear` and `tag rm` on a file with no row whose own value they
    empty report the drop, in a dry run as well as for real."""
    home, w = fresh(workspace, "r3-rowless-drop")
    if not carries_xattrs(w):
        return
    cleared, removed, bare = w / "cleared", w / "removed", w / "bare"
    for path in (cleared, removed, bare):
        path.write_text("x")
    os.setxattr(cleared, "user.omafiles.tags", b"foo,Bad Tag")
    os.setxattr(removed, "user.omafiles.tags", b"foo")
    shown = run(home, "-n", "tag", "clear", "--", cleared, bare)
    assert shown.returncode == 0 and shown.stdout.splitlines() == [str(cleared)], shown
    assert run(home, "tag", "clear", "--", cleared, bare).returncode == 0
    assert "user.omafiles.tags" not in os.listxattr(cleared)
    os.setxattr(cleared, "user.omafiles.tags", b"foo")
    receipt = json.loads(run(home, "-n", "--json", "tag", "clear", "--", cleared).stdout)
    assert [(r["path"], r["tags"]) for r in receipt] == [(str(cleared), [])], receipt
    dropped = run(home, "--json", "tag", "rm", "--tag", "foo", "--", removed, bare)
    assert dropped.returncode == 0, dropped.stderr
    assert [(r["path"], r["tags"]) for r in json.loads(dropped.stdout)] == [(str(removed), [])], \
        dropped.stdout
    assert "user.omafiles.tags" not in os.listxattr(removed)


def check_a_batch_line_starts_from_what_an_earlier_line_settled(data, workspace):
    """A path an earlier line of a batch settled starts from its row, not from
    the copy on disk the flush is about to rewrite."""
    home, w = fresh(workspace, "r3-batch-settled")
    if not carries_xattrs(w):
        return
    c = w / "c"
    c.write_text("c")
    os.setxattr(c, "user.omafiles.tags", b"old")
    lines = f"tag clear -- {c}\ntag add --tag y -- {c}\n"
    for globals_ in (["-n", "--json"], ["--json"]):
        done = run(home, *globals_, "batch", stdin=lines)
        assert done.returncode == 0, done.stderr
        assert [r["tags"] for r in json.loads(done.stdout)] == [["y"]], (globals_, done.stdout)
    assert os.getxattr(c, "user.omafiles.tags") == b"y"

    # A walk inside the batch reads a settled path's row, not the value its
    # flush is about to replace. The row took the file's `old` in when the
    # line began, as every tag write does.
    os.setxattr(c, "user.omafiles.tags", b"old")
    index = home / "omafiles" / "index.jsonl"
    for lines, rows in ((f"tag add --tag z -- {c}\nreindex --from-xattr {w}\n",
                         [["old", "y", "z"]]),
                        (f"tag clear -- {c}\nreindex {w}\n", [])):
        assert run(home, "batch", stdin=lines).returncode == 0, lines
        assert [json.loads(row)["tags"] for row in index.read_text().splitlines()] == rows, \
            (lines, index.read_text())


def check_a_copy_carries_the_files_own_value(data, workspace):
    """`cp`, and `mv` across a filesystem, carry the source's own value as it
    stands, the bytes a rename keeps, with or without a row."""
    home, w = fresh(workspace, "r3-copy-value")
    if not carries_xattrs(w):
        return
    plain, mixed = w / "plain", w / "mixed"
    plain.write_text("p")
    mixed.write_text("m")
    os.setxattr(plain, "user.omafiles.tags", b"foo,bar")
    os.setxattr(mixed, "user.omafiles.tags", b"foo,Bad Tag")
    for source in (plain, mixed):
        copied = w / (source.name + ".copy")
        done = run(home, "cp", source, copied)
        assert done.returncode == 0, done.stderr
        assert os.getxattr(copied, "user.omafiles.tags") == os.getxattr(source, "user.omafiles.tags")
    assert run(home, "json", "--", w / "plain.copy").stdout == "", "a copy of no row made one"

    # Across a filesystem: a tmpfs over `other`, read back before it goes.
    bwrap, getfattr = shutil.which("bwrap"), shutil.which("getfattr")
    if not bwrap or not getfattr:
        return
    other = w / "other"
    other.mkdir()
    crossing = w / "crossing"
    crossing.write_text("c")
    os.setxattr(crossing, "user.omafiles.tags", b"foo,bar")
    script = '"$0" mv "$1" "$2/crossing" && "$3" --only-values -n user.omafiles.tags "$2/crossing"'
    moved = subprocess.run(
        [bwrap, "--dev-bind", "/", "/", "--tmpfs", str(other), "sh", "-c", script,
         str(CLI), str(crossing), str(other), getfattr],
        env=cli_env(home), capture_output=True, text=True, stdin=subprocess.DEVNULL)
    assert moved.returncode == 0 and moved.stdout.endswith("foo,bar"), (moved.stdout, moved.stderr)


def check_a_kept_file_is_read_in_the_shape_it_is_written(data, workspace):
    """The index, the roots and the views are read back only in the shape this
    program writes: a byte order mark on any line, a row's tags missing, empty,
    unsorted or doubled, an unclean root path and two ids for one path are each
    exit 3 naming the line. A key given twice keeps its last value, as jq does."""
    home, w = fresh(workspace, "r3-kept-shape")
    f = w / "f"
    f.write_text("f")
    index = home / "omafiles" / "index.jsonl"
    index.parent.mkdir(parents=True)
    row = json.dumps({"path": str(f), "tags": ["a"]}).encode()
    for body in (b"\xef\xbb\xbf" + row, b'{"path":"%s"}' % str(f).encode(),
                 *(b'{"path":"%s","tags":%s}' % (str(f).encode(), tags)
                   for tags in (b"[]", b'["b","a"]', b'["a","a"]'))):
        index.write_bytes(body + b"\n")
        refused = run(home, "tag", "ls", "--", f)
        assert refused.returncode == 3 and f"{index}:1" in refused.stderr, (body, refused.stderr)
    index.write_bytes(b'{"path":"/elsewhere","path":"%s","tags":["a"]}\n' % str(f).encode())
    last = run(home, "tag", "ls", "--", f)
    assert last.returncode == 0 and last.stdout == "a\n", (last.returncode, last.stderr)
    index.unlink()

    dump = w / "dump"
    dump.write_bytes(row + b"\n\xef\xbb\xbf" + row + b"\n")
    bom = run(home, "-n", "import", dump)
    assert bom.returncode == 1 and "line 2" in bom.stderr and "byte order" in bom.stderr, \
        bom.stderr
    dump.write_bytes(b'{"path":"/q","tags":["\xff"]}\n')
    bytes_ = run_bytes(home, "-n", "import", dump, extra_env={b"XDG_DATA_HOME": os.fsencode(home)})
    assert bytes_.returncode == 1 and b"not valid UTF-8" in bytes_.stderr, bytes_.stderr

    config = home / "config" / "omafiles"
    config.mkdir(parents=True)
    roots = config / "roots.jsonl"
    for body in ({"id": "a", "path": f"{w}/"}, {"id": "a", "path": f"{w}/x/../"},
                 [{"id": "a", "path": str(w)}, {"id": "b", "path": str(w)}]):
        roots.write_text("".join(json.dumps(r) + "\n" for r in
                                 (body if isinstance(body, list) else [body])))
        refused = run(home, "ls")
        assert refused.returncode == 3 and str(roots) in refused.stderr, (body, refused.stderr)
    roots.unlink()

    views = config / "views.jsonl"
    for body in ({"name": "v", "tags": ["Bad Tag"]}, {"name": "v", "tags": ["b", "a"]},
                 {"name": "v", "dir": str(w), "tags": ["a"]}, {"name": "v", "dir": "rel"},
                 [{"name": "a", "tags": ["x"]}, {"name": "A", "tags": ["y"]}]):
        views.write_text("".join(json.dumps(v) + "\n" for v in
                                 (body if isinstance(body, list) else [body])))
        refused = run(home, "view", "ls")
        assert refused.returncode == 3 and str(views) in refused.stderr, (body, refused.stderr)
    views.unlink()
    tabbed = run(home, "view", "add", "--name", "a\tb", "--tag", "a")
    assert tabbed.returncode == 1 and "a\\tb" in tabbed.stderr, tabbed.stderr
    assert not views.exists()
    if os.geteuid() != 0:
        # A directory it cannot make is named with the system's reason.
        config.rmdir()
        config.parent.chmod(0o555)
        try:
            shut = run(home, "view", "add", "--name", "v", "--tag", "a")
        finally:
            config.parent.chmod(0o755)
        assert shut.returncode == 3 and "Permission denied" in shut.stderr, shut.stderr


def check_a_batch_line_splits_at_spaces_and_tabs_only(data, workspace):
    """A batch line splits where the default IFS does, loses one `\\r` before
    its newline and no other, and is parsed whole before any line runs."""
    home, w = fresh(workspace, "r3-batch-split")
    names = ["b\r", "a b", "x\ry", "v\x0bw", "e\r"]
    for name in names:
        (w / name).write_text("n")
    body = (f"tag add -t t -- {w}/b\r\r\n" f"tag add -t t -- {w}/a b\n"
            f"tag add -t t -- {w}/x\ry\n" f"tag add -t t -- {w}/v\x0bw\n"
            f"tag add -t t -- {w}/e\r")
    done = run_bytes(home, "--json", "batch", stdin=body.encode(),
                     extra_env={b"XDG_DATA_HOME": os.fsencode(home)})
    assert done.returncode == 0, done.stderr
    assert sorted(r["path"] for r in json.loads(done.stdout)) == sorted(f"{w}/{n}" for n in names)

    unknown = run(home, "batch", stdin=f"tag add -t q -- {w}/missing\nfoo\n")
    assert unknown.returncode == 1 and "line 2" in unknown.stderr, unknown.stderr
    assert "unknown command: foo" in unknown.stderr, unknown.stderr
    assert "missing" not in unknown.stderr, unknown.stderr
    first = w / "first"
    first.write_text("f")
    same = run(home, "batch", stdin=f"tag add -t q -- {first}\nimport /dev/stdin\n")
    assert same.returncode == 1 and "line 2" in same.stderr and "stdin" in same.stderr, \
        same.stderr
    assert run(home, "json", "--", first).stdout == ""


def check_completion_hands_over_the_words_up_to_the_cursor(data, workspace):
    """The bash and zsh shims pass the words up to the cursor, not the whole
    line; a global on the line is not offered again, and a tag is offered after
    `--tag` only where a verb takes one."""
    home, _ = fresh(workspace, "r3-complete-cursor")
    bash = run(home, "completions", "bash").stdout
    assert "for ((i = 1; i <= COMP_CWORD; i++)); do" in bash, bash
    zsh = run(home, "completions", "zsh").stdout
    assert '"${(@)words[2,CURRENT]}"' in zsh, zsh
    top = run(home, "__complete", "--", "--json", "").stdout.split()
    assert "tag" in top and "--json" not in top and "--yes" in top, top
    flagged = run(home, "__complete", "--", "ls", "--json", "-").stdout.split()
    assert "--tag" in flagged and "--json" not in flagged, flagged
    assert run(home, "tag", "add", "--tag", "work", "--", home / "w").returncode == 0
    attached = run(home, "__complete", "--", "--tag=w")
    assert "--tag=work" not in attached.stdout, attached.stdout
    assert "--tag=work" in run(home, "__complete", "--", "ls", "--tag=w").stdout


def check_what_a_run_says_about_itself_is_true(data, workspace):
    """A flag before the verb is called a flag, `tag merge` says how many tags
    it takes, a bad tag is named with its verb, a count past `int` is a count,
    and a value's bytes reach no stdout raw."""
    home, w = fresh(workspace, "r3-true-words")
    for word, called in (("--json=1", "--json takes no value"), ("-jn", "not a global flag")):
        said = run(home, word, "tags")
        assert said.returncode == 1 and called in said.stderr, said.stderr
        assert "unknown command" not in said.stderr, said.stderr
    helped = run(home, "__complete", "--help")
    assert helped.returncode == 1 and "unknown command" not in helped.stderr, helped.stderr
    three = run(home, "tag", "merge", "a", "b", "c")
    assert three.returncode == 1 and "exactly two tags" in three.stderr, three.stderr
    bad = run(home, "tag", "add", "--tag", "Bad Tag", "--", w)
    assert bad.stderr.startswith("tag add: invalid tag"), bad.stderr
    f = w / "f"
    f.write_text("f")
    assert run(home, "tag", "add", "--tag", "a", "--", f).returncode == 0
    for count in ("2147483648", "99999999999999999999"):
        wide = run(home, "recent", count)
        assert wide.returncode == 0 and wide.stdout == f"{f}\n", (count, wide.stdout, wide.stderr)
    if not carries_xattrs(w):
        return
    loose = w / "loose"
    loose.write_text("l")
    os.setxattr(loose, "user.omafiles.tags", b"a\x01b")
    os.setxattr(f, "user.omafiles.tags", b"a\x00b")
    checked = run_bytes(home, "verify", w, extra_env={b"XDG_DATA_HOME": os.fsencode(home)})
    assert b"\x00" not in checked.stdout and b"\x01" not in checked.stdout, checked.stdout
    assert b"xattr=a\\x00b" in checked.stdout and b"xattr=a\\x01b" in checked.stdout, \
        checked.stdout


def check_a_tag_is_folded_in_ascii_only(data, workspace):
    """Only ASCII letters are folded: the Kelvin sign is not `k`, and a
    trailing newline is not trimmed into a tag."""
    home, w = fresh(workspace, "r3-ascii-fold")
    for word in ("K", "work\n", " work"):
        refused = run(home, "tag", "add", "--tag", word, "--", w)
        assert refused.returncode == 1 and "invalid tag" in refused.stderr, (word, refused.stderr)
    folded = run(home, "--json", "tag", "add", "--tag", " WORK\t", "--", w)
    assert json.loads(folded.stdout)[0]["tags"] == ["work"], folded.stdout
    # A root id is folded by the same rule.
    for word in ("\u212a", "a\n"):
        refused = run(home, "roots", "add", "--id", word, "--", w)
        assert refused.returncode == 1 and "is not an id" in refused.stderr, (word, refused)
    assert run(home, "roots", "add", "--id", " A\t", "--", w).returncode == 0
    assert run(home, "roots").stdout.split("\t")[0] == "a"


def check_a_flag_from_the_environment_is_on_or_off(data, workspace):
    """An `OMAFILES_*` flag outside the two vocabularies stops the run: a typo
    in `OMAFILES_DRY_RUN` must not write."""
    home, w = fresh(workspace, "r3-env-flags")
    typo = run(home, "tag", "add", "--tag", "t", "--", w, extra_env={"OMAFILES_DRY_RUN": "ture"})
    assert typo.returncode == 1 and "OMAFILES_DRY_RUN=ture" in typo.stderr, typo.stderr
    assert run(home, "export").stdout == ""
    off = run(home, "tag", "add", "--tag", "t", "--", w, extra_env={"OMAFILES_DRY_RUN": "Off"})
    assert off.returncode == 0 and "t" in run(home, "tag", "ls", "--", w).stdout, off.stderr
    on = run(home, "tags", extra_env={"OMAFILES_JSON": "ON"})
    assert json.loads(on.stdout) == [{"tag": "t", "count": 1}], on.stdout


def check_tag_merge_onto_itself_is_refused(data, workspace):
    """`tag merge a A` renames nothing, so it is refused as `mv a a` is."""
    home, w = fresh(workspace, "r3-merge-self")
    assert run(home, "tag", "add", "--tag", "a", "--", w).returncode == 0
    before = run(home, "export").stdout
    same = run(home, "--json", "tag", "merge", "a", "A")
    assert same.returncode == 1 and same.stdout == "" and "same tag" in same.stderr, same
    assert run(home, "export").stdout == before


def check_landing_on_a_stale_row_names_the_tags_it_drops(data, workspace):
    """A free destination with a row is a row whose file is gone: `mv` and
    `cp` drop it, name its tags, and hand them to nothing."""
    home, w = fresh(workspace, "r3-stale-row")
    for verb in ("mv", "cp"):
        gone, source = w / f"{verb}-gone", w / f"{verb}-source"
        gone.write_text("g")
        source.write_text("s")
        assert run(home, "tag", "add", "--tag", "stale", "--", gone).returncode == 0
        gone.unlink()
        landed = run(home, verb, source, gone)
        assert landed.returncode == 0, landed.stderr
        assert "stale" in landed.stderr and str(gone) in landed.stderr, landed.stderr
        assert run(home, "json", "--", gone).stdout == "", verb


def check_trash_lists_each_operand_before_its_rows(data, workspace):
    """`removed` is each operand, then the rows under it, whether or not the
    folder had a row of its own."""
    if shutil.which("gio") is None:
        return
    home, w = fresh(workspace, "r3-trash-order")
    d, e = w / "d", w / "e"
    for folder in (d, e):
        folder.mkdir()
        (folder / "x").write_text("x")
    assert run(home, "tag", "add", "--tag", "t", "--", d, d / "x", e / "x").returncode == 0
    planned = run(home, "-n", "--json", "trash", "--", d, e)
    assert planned.returncode == 0, planned.stderr
    assert json.loads(planned.stdout)["removed"] == [str(d), str(d / "x"), str(e), str(e / "x")], \
        planned.stdout


def check_no_write_goes_through_a_link_at_a_rows_path(data, workspace):
    """A row whose path is now a symlink is never written through: the file
    the link leads to keeps its own tags."""
    home, w = fresh(workspace, "r3-no-write-through")
    if not carries_xattrs(w):
        return
    a, b = w / "a", w / "b"
    a.write_text("a")
    b.write_text("b")
    assert run(home, "tag", "add", "--tag", "x", "--", a).returncode == 0
    assert run(home, "tag", "add", "--tag", "other", "--", b).returncode == 0
    a.unlink()
    a.symlink_to(b)
    run(home, "tag", "merge", "x", "y")
    assert os.getxattr(b, "user.omafiles.tags") == b"other"


def check_import_starts_from_the_files_own_tags(data, workspace):
    """A file that is here with no row keeps the tags its own value carries
    when a dump is merged onto it."""
    home, w = fresh(workspace, "r3-import-adopts")
    if not carries_xattrs(w):
        return
    f = w / "f"
    f.write_text("f")
    os.setxattr(f, "user.omafiles.tags", b"foo")
    (w / "dump").write_text(json.dumps({"path": str(f), "tags": ["bar"]}) + "\n")
    merged = run(home, "--json", "import", w / "dump")
    assert merged.returncode == 0, merged.stderr
    assert json.loads(merged.stdout)[0]["tags"] == ["bar", "foo"], merged.stdout
    assert os.getxattr(f, "user.omafiles.tags") == b"bar,foo"


def check_a_lock_nobody_wrote_is_named(data, workspace):
    """A lock file no omafiles wrote is named as that, not as another writer."""
    home, w = fresh(workspace, "r3-foreign-lock")
    (home / "omafiles").mkdir()
    (home / "omafiles" / "index.jsonl.lock").write_text("")
    held = run(home, "tag", "add", "--tag", "t", "--", w)
    assert held.returncode == 3 and "no omafiles wrote" in held.stderr, held.stderr
    assert "another writer" not in held.stderr, held.stderr


def fake_gio(home, refuse='[ -e "$2" ] || { echo "$2: gone" >&2; exit 1; }'):
    """A bwrap prefix putting a stand-in for `gio trash` in place of `gio`: it
    deletes its operand, and fails where `refuse` exits, by default on one that
    is not there. None without bwrap or gio. PATH is left alone."""
    bwrap, found = shutil.which("bwrap"), shutil.which("gio")
    if not bwrap or not found:
        return None
    stand_in = home / "gio"
    stand_in.write_text(f'#!/bin/sh\n{refuse}\nrm -rf -- "$2"\n')
    stand_in.chmod(0o755)
    return [bwrap, "--dev-bind", "/", "/", "--ro-bind", str(stand_in),
            os.path.realpath(found)]


def check_r4_an_away_root_inside_an_operand_refuses_it(data, workspace):
    """`rm`, `trash` and `mv` refuse a tree holding a root that is away, and
    `mv` and `cp` a destination that is one: its rows are not the tree's."""
    home, w = fresh(workspace, "r4-away-inside")
    (w / "o" / "in").mkdir(parents=True)
    for name in ("o/a", "o/in/b", "o/in/.mk", "c"):
        (w / name).write_text("x")
    assert run(home, "tag", "add", "--tag", "t", "--", w / "o/a", w / "o/in/b").returncode == 0
    assert run(home, "roots", "add", "--id", "inner", "--check", ".mk", w / "o/in").returncode == 0
    assert run(home, "roots", "migrate").returncode == 0
    (w / "o/in/.mk").unlink()
    (w / "o/in/b").unlink()
    index = home / "omafiles" / "index.jsonl"
    before = index.read_text()
    said = f"{w}/o holds {w}/o/in: root inner is declared and not here"
    wrap = fake_gio(home)
    runs = [(("-n", "--json", "trash", w / "o"), "trash"),
            (("mv", w / "o", w / "o2"), "mv"), (("-n", "--json", "mv", w / "o", w / "o2"), "mv"),
            (("-n", "mv", "--replace", "o", "--with", "p", w / "o"), "mv")]
    if wrap:
        runs.append((("-y", "--json", "trash", w / "o"), "trash"))
    for args, verb in runs:
        refused = run(home, *args, wrap=wrap if "-y" in args else ())
        assert refused.returncode == 1, (args, refused.stdout, refused.stderr)
        assert f"{verb}: {said}" in refused.stderr, (args, refused.stderr)
        assert refused.stdout == "" and index.read_text() == before, (args, refused.stdout)
        assert (w / "o/a").exists(), args
    # A destination that is the away root is not free for its rows to drop.
    for args in (("mv", w / "c", w / "o/in"), ("cp", w / "o/a", w / "o/in")):
        (w / "o/in").rename(w / "gone")
        try:
            refused = run(home, *args)
        finally:
            (w / "gone").rename(w / "o/in")
        assert refused.returncode == 1, (args, refused.stderr)
        assert "root inner is declared and not here" in refused.stderr, (args, refused.stderr)
        assert index.read_text() == before, args


def check_r4_a_link_at_a_walked_rows_path_is_untagged(data, workspace):
    """A walk joins an entry to its row only where the row is the file: a link
    now at a row's path is untagged, as `has` and `verify` read it."""
    home, w = fresh(workspace, "r4-walk-link")
    (w / "p").write_text("p")
    (w / "q").write_text("q")
    assert run(home, "tag", "add", "--tag", "t", "--", w / "p", w / "q").returncode == 0
    (w / "p").unlink()
    (w / "p").symlink_to("q")
    untagged = run(home, "ls", "--untagged", w)
    assert untagged.stdout.splitlines() == [str(w / "p")], untagged.stdout
    named = run(home, "--json", "ls", "--name", "p*", w)
    assert json.loads(named.stdout) == [{"path": str(w / "p"), "tags": []}], named.stdout


def check_r4_a_cached_answer_agrees_with_the_walk(data, workspace):
    """`--cached` names a gap at `[dir]` itself, reads a list stamped in the
    future as new, and both answers list an away nested root's directory."""
    home, w = fresh(workspace, "r4-cached-agrees")
    r = w / "r"
    (r / "inner").mkdir(parents=True)
    for name in ("a", "inner/.here"):
        (r / name).write_text("x")
    assert run(home, "roots", "add", "--id", "r", r).returncode == 0
    added = run(home, "roots", "add", "--id", "inner", "--check", ".here", r / "inner")
    assert added.returncode == 0, added.stderr
    if os.geteuid() != 0:
        (r / "sh").mkdir()
        for shut in ("shut", "sh x"):
            (r / shut).mkdir()
            (r / shut).chmod(0)
        try:
            run(home, "sweep")
            for scope in (r / "shut", "shut", "sh x"):
                gap = run(home, "ls", "--untagged", "--cached", scope, cwd=r)
                assert gap.returncode == 1, (scope, gap.stderr)
                assert f"{r / scope} is unreadable" in gap.stderr, (scope, gap.stderr)
            # A sibling is not the gap, though a gap's sentence begins with its name.
            elsewhere = run(home, "ls", "--untagged", "--cached", r / "sh")
            assert elsewhere.returncode == 0 and "unreadable" not in elsewhere.stderr, \
                elsewhere.stderr
        finally:
            for shut in ("shut", "sh x"):
                (r / shut).chmod(0o755)
                (r / shut).rmdir()
            (r / "sh").rmdir()
    assert run(home, "sweep").returncode == 0
    names = home / "cache" / "omafiles" / "names" / "r"
    ahead = time.time() + 120
    os.utime(names, (ahead, ahead))
    future = run(home, "ls", "--untagged", "--cached", r)
    assert future.returncode == 0, future.stderr
    assert future.stdout.splitlines() == [str(r / "a"), str(r / "inner"),
                                          str(r / "inner" / ".here")], future.stdout
    (r / "inner" / ".here").unlink()
    for cached in ((), ("--cached",)):
        away = run(home, "ls", "--untagged", *cached, r)
        assert away.returncode == 1, (cached, away.stderr)
        assert away.stdout.splitlines() == [str(r / "a"), str(r / "inner")], (cached, away.stdout)
        assert "root inner is declared and not here" in away.stderr, (cached, away.stderr)


def check_r4_filters_follow_their_prior_art(data, workspace):
    """`--type f` lists a dangling link and no link the system will not
    describe; a suffix follows Python's `splitext`, for `--ext` and `{ext}`;
    `recent` refuses a `#` as GLib does and a count with trailing blanks as
    `head -n` does."""
    home, w = fresh(workspace, "r4-prior-art")
    for name in ("f", "..d", ".a.b"):
        (w / name).write_text("x")
    (w / "loop").symlink_to("loop")
    (w / "dang").symlink_to("nowhere")
    expected = [w / "..d", w / ".a.b", w / "dang", w / "f"]
    if os.geteuid() != 0:
        (w / "shut").mkdir()
        (w / "shut" / "t").write_text("t")
        (w / "acc").symlink_to("shut/t")
        (w / "shut").chmod(0)
    try:
        typed = run(home, "ls", "--untagged", "--type", "f", w)
    finally:
        if (w / "shut").exists():
            (w / "shut").chmod(0o755)
    assert [line for line in typed.stdout.splitlines()] == [str(p) for p in expected], typed.stdout
    assert run(home, "ls", "--untagged", "--ext", "d", w).stdout == "", "..d has no suffix"
    assert run(home, "ls", "--untagged", "--ext", "b", w).stdout.splitlines() == [str(w / ".a.b")]
    planned = run(home, "-n", "mv", "--pattern", "{stem}X{ext}", w / "..d", w / ".a.b")
    assert planned.stdout.splitlines() == [f"{w}/..d\t{w}/..dX", f"{w}/.a.b\t{w}/.aX.b"], \
        planned.stdout
    (home / "recently-used.xbel").write_text(f"""<?xml version="1.0"?>
<xbel version="1.0"><bookmark href="file://{w}/f#frag" visited="2026-09-03T10:00:00Z"/>
<bookmark href="file://{w}/.a.b" visited="2026-09-02T10:00:00Z"/></xbel>
""")
    desktop = run(home, "recent", "--desktop")
    assert desktop.stdout.splitlines() == [str(w / ".a.b")], desktop.stdout
    trailing = run(home, "recent", "3 ")
    assert trailing.returncode == 1 and "is not a count" in trailing.stderr, trailing.stderr
    assert run(home, "recent", " +3").returncode == 0


def check_r4_a_bind_mount_is_a_mount(data, workspace):
    """A `--mount` root is here where the kernel's mount table has it, a bind
    from the same filesystem and a name the table escapes included."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        return
    home, w = fresh(workspace, "r4-bind")
    for name in ("real", "sp ace", "t", "t2"):
        (w / name).mkdir()
    assert run(home, "roots", "add", "--id", "m", "--mount", w / "real").returncode == 0
    assert run(home, "roots", "add", "--id", "s", "--mount", w / "sp ace").returncode == 0
    assert [line.split("\t")[2] for line in run(home, "roots").stdout.splitlines()] == \
        ["away", "away"]
    bound = run(home, "roots", wrap=[bwrap, "--dev-bind", "/", "/", "--bind", w / "t",
                                     w / "real", "--bind", w / "t2", w / "sp ace"])
    assert [line.split("\t")[2] for line in bound.stdout.splitlines()] == ["here", "here"], \
        (bound.stdout, bound.stderr)


def check_r4_a_row_under_a_linked_directory_is_missing(data, workspace):
    """A row counts only where resolving its path gives it back: a directory
    above it that is now a symlink makes it missing, and nothing lists it."""
    home, w = fresh(workspace, "r4-linked-parent")
    (w / "dir").mkdir()
    (w / "dir" / "f").write_text("f")
    assert run(home, "tag", "add", "--tag", "t", "--", w / "dir/f").returncode == 0
    (w / "dir").rename(w / "dir2")
    (w / "dir").symlink_to("dir2")
    assert run(home, "ls", "--tag", "t").stdout == ""
    assert run(home, "tags").stdout == ""
    checked = run(home, "verify", w)
    assert f"missing\t{w}/dir/f" in checked.stdout.splitlines(), checked.stdout
    pruned = run(home, "--yes", "prune", w)
    assert pruned.stdout.splitlines() == [f"{w}/dir/f"], (pruned.stdout, pruned.stderr)


def check_r4_dot_and_root_operands_are_refused(data, workspace):
    """`rm`, `trash` and `mv` refuse an operand ending in `.` or `..`, as POSIX
    `rm` does, and `mv` refuses `/`; nothing is done."""
    home, w = fresh(workspace, "r4-dots")
    (w / "d" / "e").mkdir(parents=True)
    here = w / "d" / "e"
    for args in (("-n", "trash", "."), ("-n", "trash", "./"), ("rm", ".."),
                 ("mv", ".", "../moved"), ("-n", "mv", "..", "x"),
                 ("mv", "--replace", "e", "--with", "z", "."), ("-n", "rm", "../e/..")):
        refused = run(home, *args, cwd=here)
        assert refused.returncode == 1, (args, refused.stdout, refused.stderr)
        assert "ends in `.` or `..`" in refused.stderr, (args, refused.stderr)
        assert refused.stdout == "" and here.is_dir(), args
    root = run(home, "-n", "mv", "/", w / "r")
    assert root.returncode == 1 and "mv: / is the whole filesystem" in root.stderr, root.stderr


def check_r4_a_fifo_does_not_cross_in_a_dry_run(data, workspace):
    """A fifo that would cross a filesystem is refused by the dry run as by the
    real run."""
    shm = Path("/dev/shm")
    home, w = fresh(workspace, "r4-fifo-cross")
    if not shm.is_dir() or os.stat(shm).st_dev == os.stat(w).st_dev:
        return
    os.mkfifo(w / "ff")
    landing = shm / "omafiles-r4-fifo"
    for dry in (("-n",), ()):
        refused = run(home, *dry, "mv", w / "ff", landing)
        assert refused.returncode == 1, (dry, refused.stdout, refused.stderr)
        assert "is not a regular file" in refused.stderr, (dry, refused.stderr)
        assert refused.stdout == "" and not landing.exists(), dry


def check_r4_trash_takes_an_operand_inside_another_once(data, workspace):
    """`trash z z/y/f` is `trash z`, and `removed` lists what went."""
    home, w = fresh(workspace, "r4-trash-nested")
    wrap = fake_gio(home)
    if not wrap:
        return
    (w / "z" / "y").mkdir(parents=True)
    (w / "z" / "y" / "f").write_text("f")
    assert run(home, "tag", "add", "--tag", "t", "--", w / "z/y/f").returncode == 0
    gone = run(home, "-y", "--json", "trash", w / "z", w / "z/y/f", wrap=wrap)
    assert gone.returncode == 0, (gone.stdout, gone.stderr)
    assert json.loads(gone.stdout)["removed"] == [str(w / "z"), str(w / "z/y/f")], gone.stdout
    assert not (w / "z").exists() and run(home, "export").stdout == ""


def check_r4_every_diagnostic_names_its_verb(data, workspace):
    """An operand or a `[dir]` the system refuses is said with the verb."""
    home, w = fresh(workspace, "r4-verb-named")
    (w / "f").write_text("f")
    for args, said in ((("-n", "trash", f"{w}/f/"), "trash: "),
                       (("mv", f"{w}/f/", w / "g"), "mv: "),
                       (("-n", "rm", w / "nope"), "rm: path missing: "),
                       (("tag", "add", "-t", "x", w / "nope"), "tag add: path missing: "),
                       (("ls", w / "nope"), "ls: path missing: "),
                       (("tags", w / "f"), "tags: not a directory: "),
                       (("verify", w / "f"), "verify: not a directory: "),
                       (("chmod", "644", w / "nope"), "chmod: path missing: ")):
        refused = run(home, *args)
        assert refused.returncode != 0 and refused.stderr.startswith(said), (args, refused.stderr)


def check_r4_a_dangling_link_is_named_hop_by_hop(data, workspace):
    """A link that leads nowhere, at the end of a path or in the middle of one,
    and as the file `batch` or `import` reads, is named hop by hop."""
    home, w = fresh(workspace, "r4-hops")
    (w / "L").symlink_to("nowhere")
    (w / "D2").symlink_to("L")
    for args, said in ((("batch", w / "L"), f"batch: {w}/L is a symlink to {w}/nowhere"),
                       (("import", w / "L"), f"import: {w}/L is a symlink to {w}/nowhere"),
                       (("json", w / "L/.."), f"json: {w}/L/..: {w}/L is a symlink to"),
                       (("json", w / "D2/x"),
                        f"json: {w}/D2/x: {w}/D2 is a symlink to {w}/L, which is a symlink to "
                        f"{w}/nowhere, which is not there")):
        refused = run(home, *args)
        assert refused.returncode == 2 and said in refused.stderr, (args, refused.stderr)


def check_r4_a_list_of_names_holds_no_blank_line(data, workspace):
    """A blank line is not a path `sweep` wrote, so the list is refused."""
    home, w = fresh(workspace, "r4-names-blank")
    (w / "a").write_text("a")
    assert run(home, "roots", "add", "--id", "r", w).returncode == 0
    assert run(home, "sweep").returncode == 0
    names = home / "cache" / "omafiles" / "names" / "r"
    names.write_text(names.read_text() + "\n")
    refused = run(home, "ls", "--untagged", "--cached")
    assert refused.returncode == 1 and "is not a clean path" in refused.stderr, refused.stderr


def check_r4_view_answers_in_the_shape_asked(data, workspace):
    """`view add --dry-run --json` prints the object the real run prints, and
    `view ls` offers the whole command line."""
    home, w = fresh(workspace, "r4-view-shape")
    planned = run(home, "-n", "--json", "view", "add", "--name", "z", "-t", "a")
    assert json.loads(planned.stdout) == {"name": "z"}, planned.stdout
    assert run(home, "view", "add", "--name", "z", "-t", "a").returncode == 0
    assert run(home, "view", "ls").stdout == "z\tomafiles ls --tag a\n"


def check_r4_sweep_counts_only_what_changed(data, workspace):
    """A file's copy the filesystem refuses is said and not counted as merged;
    one it takes is."""
    home, w = fresh(workspace, "r4-sweep-merged")
    if not carries_xattrs(w) or os.geteuid() == 0:
        return
    (w / "ro").write_text("ro")
    (w / "g").write_text("g")
    assert run(home, "tag", "add", "--tag", "t", "--", w / "ro", w / "g").returncode == 0
    for name in ("ro", "g"):
        os.removexattr(w / name, "user.omafiles.tags")
    (w / "ro").chmod(0o444)
    first = run(home, "--json", "sweep", w)
    assert json.loads(first.stdout)["merged"] == [str(w / "g")], first.stdout
    assert f"{w}/ro" in first.stderr, first.stderr
    again = run(home, "--json", "sweep", w)
    assert json.loads(again.stdout)["merged"] == [], again.stdout


def check_r4_sweep_keeps_a_receipt_when_nothing_is_here(data, workspace):
    """A run that finds every root away still says every count and keeps a
    receipt."""
    home, w = fresh(workspace, "r4-sweep-away")
    (w / ".here").write_text("")
    assert run(home, "roots", "add", "--id", "r", "--check", ".here", w).returncode == 0
    (w / ".here").unlink()
    swept = run(home, "sweep")
    assert swept.returncode == 0 and "adopted 0, merged 0" in swept.stderr, swept.stderr
    status = run(home, "sweep", "--status")
    assert status.stdout.startswith("at\t"), status.stdout


def check_r4_sweep_does_not_wait_for_another_writer(data, workspace):
    """Meeting another writer's lock, `sweep` steps aside at once."""
    home, w = fresh(workspace, "r4-sweep-nowait")
    (home / "omafiles").mkdir()
    lock = forge_lock(home)
    try:
        started = time.monotonic()
        aside = run(home, "sweep", w)
        took = time.monotonic() - started
    finally:
        lock.unlink(missing_ok=True)
    assert aside.returncode == 0 and "stepped aside" in aside.stderr, aside.stderr
    assert took < 0.6, took


def check_r4_a_receipt_holds_numbers_a_clock_can_print(data, workspace):
    """A receipt count past `qint64`, or a stamp no clock can print, is not a
    receipt."""
    home, w = fresh(workspace, "r4-receipt-range")
    receipt = home / "state" / "omafiles" / "sweep.json"
    receipt.parent.mkdir(parents=True)
    counts = {k: 0 for k in ("adopted", "merged", "gone", "unreadable", "skipped", "listed",
                             "unlistable")}
    for at, adopted in (("1e300", "0"), ("9000000000000000000", "0"), ("1", "1e20")):
        body = json.dumps({**counts, "at": 0, "seconds": 1, "trees": []})
        receipt.write_text(body.replace('"at": 0', f'"at": {at}')
                           .replace('"adopted": 0', f'"adopted": {adopted}'))
        refused = run(home, "sweep", "--status")
        assert refused.returncode == 1 and "is not a receipt" in refused.stderr, \
            (at, adopted, refused.stdout, refused.stderr)


def check_a_value_holding_a_non_tag_is_only_tag_clears(data, workspace):
    """A value holding a word that is not a tag is left as it is by every
    writer but `tag clear`, however many writes follow, and each says so."""
    home, w = fresh(workspace, "r4-untouchable")
    if not carries_xattrs(w):
        return
    q, b, n, m = (w / name for name in ("q", "b", "n", "m"))
    for path, value in ((q, b"foo,Bad Tag"), (b, b"Bad Tag"), (n, b"ok,Bad Tag"),
                        (m, b"old,Bad Tag")):
        path.write_text(path.name)
        os.setxattr(path, "user.omafiles.tags", value)
    for words in (("tag", "add", "-t", "z", "--", q, b), ("tag", "add", "-t", "y", "--", q),
                  ("tag", "rm", "-t", "z", "--", q), ("tag", "cp", "--from", q, "--", b),
                  ("tag", "merge", "old", "new"), ("import", w / "dump")):
        if words[1:3] == ("merge", "old"):
            # Its valid tags are already the row's, so there is nothing to refuse.
            kept = run(home, "tag", "add", "-t", "old", "--", m)
            assert kept.returncode == 0 and kept.stderr == "", kept
        if words[0] == "import":
            (w / "dump").write_text(json.dumps({"path": str(q), "tags": ["d"]}) + "\n")
        done = run(home, *words)
        assert done.returncode == 0 and "left as it is" in done.stderr, (words, done)
    # The refusal is this program's own, so a dry run makes it too.
    previewed = run(home, "-n", "tag", "add", "-t", "p", "--", q)
    assert previewed.returncode == 0 and "left as it is" in previewed.stderr, previewed
    lines = f"tag add -t k -- {q}\ntag rm -t k -- {q}\n"
    batched = run(home, "batch", stdin=lines)
    assert batched.returncode == 0 and "left as it is" in batched.stderr, batched
    walked = run(home, "reindex", w)
    assert walked.returncode == 1 and "left as it is" in walked.stderr, walked
    for path, value in ((q, b"foo,Bad Tag"), (b, b"Bad Tag"), (n, b"ok,Bad Tag"),
                        (m, b"old,Bad Tag")):
        assert os.getxattr(path, "user.omafiles.tags") == value, path
    assert run(home, "tag", "ls", "--", n).stdout.split() == ["ok"]
    # `tag clear` is licensed, alone or ahead of a later line in the same batch.
    assert run(home, "batch", stdin=f"tag clear -- {q}\ntag add -t k -- {q}\n").returncode == 0
    assert os.getxattr(q, "user.omafiles.tags") == b"k"
    assert run(home, "tag", "clear", "--", b).returncode == 0
    assert "user.omafiles.tags" not in os.listxattr(b)


def check_a_run_answers_for_the_state_after_the_flush(data, workspace):
    """A drop the filesystem refuses on a path no row records is no receipt
    and exit 1, alone or in a batch; a path a `reindex` line walked is promised
    whatever line queued its copy."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r4-after-flush")
    if not carries_xattrs(w):
        return
    v, g, f = w / "v", w / "g", w / "ro" / "f"
    f.parent.mkdir()
    for path in (v, g, f):
        path.write_text("x")
    for path in (v, g):
        os.setxattr(path, "user.omafiles.tags", b"keep")
    for path in (v, g, f):
        path.chmod(0o444)
    try:
        for words in (("tag", "clear", "--", v), ("tag", "rm", "-t", "keep", "--", v)):
            done = run(home, "--json", *words)
            assert done.returncode == 1 and json.loads(done.stdout) == [], (words, done)
        added = run(home, "batch", stdin=f"tag add --tag t -- {g}\ntag clear -- {g}\n")
        assert added.returncode == 1 and "Permission denied" in added.stderr, added
        walked = run(home, "batch", stdin=f"tag add --tag u -- {f}\nreindex {f.parent}\n")
        assert walked.returncode == 1 and "Permission denied" in walked.stderr, walked
    finally:
        for path in (v, g, f):
            path.chmod(0o644)
    for path in (v, g):
        assert os.getxattr(path, "user.omafiles.tags") == b"keep", path


def check_an_empty_value_is_none_and_a_comma_is_invalid(data, workspace):
    """A value of no bytes is no value; `,` is a value no tag can be read out
    of, so it is a drop for `tag clear` and `xattr-invalid` for `verify`."""
    home, w = fresh(workspace, "r4-empty-value")
    if not carries_xattrs(w):
        return
    e, c = w / "e", w / "c"
    for path, value in ((e, b""), (c, b",")):
        path.write_text("x")
        os.setxattr(path, "user.omafiles.tags", value)
    rows = json.loads(run(home, "--json", "verify", w).stdout)["problems"]
    assert [(r["problem"], r["path"]) for r in rows] == [("xattr-invalid", str(c))], rows
    shown = run(home, "-n", "--json", "tag", "clear", "--", e, c)
    assert [r["path"] for r in json.loads(shown.stdout)] == [str(c)], shown


def check_tag_merge_restamps_what_it_renames(data, workspace):
    """`tagged` moves exactly when a row's tags change, a merge included."""
    home, w = fresh(workspace, "r4-merge-stamp")
    a = w / "a"
    a.write_text("a")
    write_index(home, [{"path": str(a), "tags": ["old"], "tagged": 1000}])
    assert run(home, "tag", "merge", "old", "new").returncode == 0
    row = json.loads(run(home, "--json", "json", "--", a).stdout)
    assert row["tags"] == ["new"] and row["tagged"] > 1000, row


def check_a_tag_write_keeps_what_the_file_carries(data, workspace):
    """Every tag write starts from the row plus the valid tags the file
    carries, so `xattr-differs` never costs a tag; `tag rm` still removes one."""
    home, w = fresh(workspace, "r4-row-and-file")
    if not carries_xattrs(w):
        return
    a = w / "a"
    a.write_text("a")
    write_index(home, [{"path": str(a), "tags": ["old"], "tagged": 1000}])
    os.setxattr(a, "user.omafiles.tags", b"extra,old")
    assert run(home, "tag", "add", "-t", "old", "--", a).returncode == 0
    row = json.loads(run(home, "--json", "json", "--", a).stdout)
    assert row["tags"] == ["extra", "old"] and row["tagged"] > 1000, row
    assert os.getxattr(a, "user.omafiles.tags") == b"extra,old"
    assert run(home, "tag", "rm", "-t", "extra", "--", a).returncode == 0
    assert os.getxattr(a, "user.omafiles.tags") == b"old"
    kept = json.loads(run(home, "--json", "json", "--", a).stdout)
    assert run(home, "tag", "rm", "-t", "never", "--", a).returncode == 0
    assert json.loads(run(home, "--json", "json", "--", a).stdout) == kept


def check_tag_cp_onto_its_source_is_refused(data, workspace):
    """`tag cp --from a a` is refused as `cp a a` is, and writes nothing."""
    home, w = fresh(workspace, "r4-cp-self")
    a, b = w / "a", w / "b"
    for path in (a, b):
        path.write_text("x")
    assert run(home, "tag", "add", "-t", "t", "--", a).returncode == 0
    before = run(home, "export").stdout
    refused = run(home, "tag", "cp", "--from", a, "--", b, a)
    assert refused.returncode == 1 and "the file the tags come from" in refused.stderr, refused
    assert run(home, "export").stdout == before


def check_a_batch_parses_every_line_before_the_first_runs(data, workspace):
    """A line whose arguments no run could take is refused before line 1 runs."""
    home, w = fresh(workspace, "r4-batch-parse")
    ok = w / "ok"
    ok.write_text("o")
    for second in ("tag merge one", 'tag add --tag "Bad Tag" -- ok', "import",
                   "reindex a b", "tag clear", "tag cp -- ok", "tag rm -- ok",
                   "prune --all x"):
        done = run(home, "batch", stdin=f"tag add --tag k -- {w}/missing\n{second}\n")
        assert done.returncode == 1, (second, done)
        assert "line 2" in done.stderr and "path missing" not in done.stderr, (second, done)


def check_a_batch_import_refuses_the_batchs_own_file(data, workspace):
    """An `import` line reading the file the batch was read out of is refused,
    whichever name either of them used for it."""
    home, w = fresh(workspace, "r4-batch-stdin")
    (w / "lines").write_text("import -\n")
    for how in ("/dev/stdin", "-", str(w / "lines")):
        done = subprocess.run([str(CLI), "--json", "batch", how], cwd=str(w),
                              env=cli_env(home), stdin=open(w / "lines", "rb"),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert done.returncode == 1 and "this batch was read out of" in done.stderr, (how, done)
        assert done.stdout == "", (how, done)
    for how, lines in (("/dev/stdin", "import -\n"), ("-", "import /dev/stdin\n")):
        done = run(home, "--json", "batch", how, stdin=lines)
        assert done.returncode == 1 and "this batch was read out of" in done.stderr, (how, done)


def check_completion_offers_only_what_the_line_takes(data, workspace):
    """Tags follow `--tag` only where the verb takes it, never past `--`, and a
    global is said once in either spelling."""
    home, w = fresh(workspace, "r4-complete")
    x = w / "x"
    x.write_text("x")
    assert run(home, "tag", "add", "-t", "one", "--", x).returncode == 0
    for words in (("mv", "--tag", ""), ("recent", "--tag", ""), ("tag", "ls", "--tag", ""),
                  ("tag", "add", "--", "--tag", ""), ("tag", "add", "--", "-")):
        done = run(home, "__complete", "--", *words)
        assert done.returncode == 1 and done.stdout == "", (words, done)
    assert run(home, "__complete", "--", "tag", "add", "--tag", "").stdout.split() == ["one"]
    for spelled in ("-j", "--json"):
        assert "--json" not in run(home, "__complete", "--", spelled, "").stdout.split()
        assert "--json" not in run(home, "__complete", "--", spelled, "ls", "-").stdout.split()


def check_a_dump_row_is_held_to_the_index_shape(data, workspace):
    """An `import` row with no tags, or empty ones, is refused naming its line,
    and nothing is merged; tags are folded, sorted and each once first."""
    home, w = fresh(workspace, "r4-dump-shape")
    q = w / "q"
    q.write_text("q")
    good = json.dumps({"path": str(q), "tags": ["q"]})
    for bad in ({"path": str(w / "r"), "tags": []}, {"path": str(w / "r")}):
        done = run(home, "import", "-", stdin=good + "\n" + json.dumps(bad) + "\n")
        assert done.returncode == 1 and "line 2" in done.stderr, (bad, done)
        assert run(home, "export").stdout == "", bad
    folded = run(home, "--json", "import", "-",
                 stdin=json.dumps({"path": str(q), "tags": ["B", "a", "a"]}) + "\n")
    assert folded.returncode == 0 and json.loads(folded.stdout)[0]["tags"] == ["a", "b"], folded
    link = w / "lnu"
    open(os.fsencode(w) + b"/\xff", "w").close()
    os.symlink(b"\xff", os.fsencode(link))
    unspelled = run(home, "import", "-", stdin=json.dumps({"path": str(link), "tags": ["x"]}))
    assert unspelled.returncode == 1 and "line 1" in unspelled.stderr, unspelled
    assert str(link) not in run(home, "export").stdout


def check_words_off_a_value_stay_one_line(data, workspace):
    """What a message quotes out of a file's value is spelled so it stays one
    line and forges nothing."""
    home, w = fresh(workspace, "r4-escaped-words")
    if not carries_xattrs(w):
        return
    f, g = w / "f", w / "g"
    for path, value in ((f, b"ok,a\x1bb"), (g, b"a\x07b")):
        path.write_text("x")
        os.setxattr(path, "user.omafiles.tags", value)
    said = run(home, "reindex", w)
    assert said.returncode == 1, said
    assert "\x1b" not in said.stderr and "\x07" not in said.stderr, said.stderr
    assert "a\\x1bb" in said.stderr and "a\\x07b" in said.stderr, said.stderr


def check_sweep_leaves_a_value_holding_a_non_tag_to_the_flush(data, workspace):
    """`sweep` leaves a value holding a word that is not a tag as it is, says so
    only where the file lacks a tag its row holds, and counts neither merged."""
    home, w = fresh(workspace, "r4-sweep-untouchable")
    if not carries_xattrs(w):
        return
    lacks, holds = w / "lacks", w / "holds"
    for path in (lacks, holds):
        path.write_text(path.name)
        os.setxattr(path, "user.omafiles.tags", b"ok,Bad Tag")
    assert run(home, "tag", "add", "-t", "more", "--", lacks).returncode == 0
    for _ in range(2):
        swept = run(home, "--json", "sweep", w)
        assert swept.returncode == 0, swept.stderr
        assert json.loads(swept.stdout)["merged"] == [], swept.stdout
        assert "lacks carries words that are not tags" in swept.stderr, swept.stderr
        assert "holds" not in swept.stderr, swept.stderr
    for path in (lacks, holds):
        assert os.getxattr(path, "user.omafiles.tags") == b"ok,Bad Tag", path


def check_r5_mv_set_renames_children_first(data, workspace):
    """`mv` over a set renames children before their parents, whatever the
    operand order, and the dry run prints the run it previews."""
    home, w = fresh(workspace, "r5-mv-set-depth")
    (w / "a").mkdir()
    (w / "a" / "a1").write_text("x")
    assert run(home, "tag", "add", "-t", "t", "--", w / "a", w / "a" / "a1").returncode == 0
    words = ("mv", "--replace", "a", "--with", "b", "--", w / "a", w / "a" / "a1")
    planned = run(home, "-n", *words)
    planned_json = run(home, "-n", "--json", *words)
    assert planned.returncode == 0 and planned_json.returncode == 0, planned.stderr
    assert planned.stdout == f"{w}/a/a1\t{w}/a/b1\n{w}/a\t{w}/b\n", planned.stdout
    done = run(home, "--json", *words)
    assert done.returncode == 0, done.stderr
    strip = lambda moves: [{**m, "rows": [r["path"] for r in m["rows"]]} for m in moves]
    assert strip(json.loads(done.stdout)) == strip(json.loads(planned_json.stdout)), done.stdout
    assert sorted(p.name for p in w.rglob("*")) == ["b", "b1"], list(w.rglob("*"))
    listed = run(home, "ls").stdout.splitlines()
    assert listed == [f"{w}/b", f"{w}/b/b1"], listed


def check_r5_rows_under_a_path_follow_only_a_folder(data, workspace):
    """A row under `a` goes with `a` only where `a` is a folder: once `a` is a
    plain file, `mv` and `rm` of it leave that row where it is."""
    home, w = fresh(workspace, "r5-rows-under-file")
    (w / "a").mkdir()
    (w / "a" / "x").write_text("x")
    assert run(home, "tag", "add", "-t", "t", "--", w / "a" / "x").returncode == 0
    (w / "a" / "x").unlink()
    (w / "a").rmdir()
    (w / "a").write_text("a")
    assert run(home, "tag", "add", "-t", "t", "--", w / "a").returncode == 0
    moved = run(home, "--json", "mv", w / "a", w / "b")
    assert moved.returncode == 0, moved.stderr
    assert [r["path"] for r in json.loads(moved.stdout)["rows"]] == [f"{w}/b"], moved.stdout
    removed = run(home, "--json", "--yes", "rm", w / "b")
    assert removed.returncode == 0, removed.stderr
    assert json.loads(removed.stdout)["removed"] == [f"{w}/b"], removed.stdout
    kept = json.loads(run(home, "export").stdout)
    assert kept["path"] == f"{w}/a/x", kept


def check_r5_sweep_sweeps_a_nested_root_once(data, workspace):
    """Roots `R` and `R/N` swept in one run count each path under `R/N` once."""
    home, w = fresh(workspace, "r5-sweep-nested")
    nested = w / "R" / "N"
    (nested / "S").mkdir(parents=True)
    for name in ("f", "g"):
        (nested / name).write_text(name)
    (w / "R" / "h").write_text("h")
    assert run(home, "roots", "add", "--id", "r", w / "R").returncode == 0
    assert run(home, "roots", "add", "--id", "n", nested).returncode == 0
    assert run(home, "tag", "add", "-t", "t", "--", nested / "f", w / "R" / "h").returncode == 0
    (nested / "f").unlink()
    if os.geteuid() != 0:
        (nested / "S").chmod(0o000)
    try:
        swept = run(home, "--json", "sweep")
    finally:
        (nested / "S").chmod(0o755)
    assert swept.returncode == 0, swept.stderr
    found = json.loads(swept.stdout)
    assert found["gone"] == [str(nested / "f")], found
    if os.geteuid() != 0:
        assert found["unreadable"] == [str(nested / "S")], found
        assert swept.stderr.count("is unreadable") == 1, swept.stderr
    assert "gone 1," in swept.stderr, swept.stderr


def check_r5_a_root_replaced_by_a_link_is_away(data, workspace):
    """A declared root whose directory is now a symlink is away: its rows are
    left alone, as for a disk in a drawer."""
    home, w = fresh(workspace, "r5-root-now-a-link")
    (w / "P").mkdir()
    (w / "big").mkdir()
    (w / "P" / "f").write_text("f")
    assert run(home, "roots", "add", "--id", "p", w / "P").returncode == 0
    assert run(home, "tag", "add", "-t", "t", "--", w / "P" / "f").returncode == 0
    (w / "P").rename(w / "big" / "P")
    (w / "P").symlink_to(w / "big" / "P")
    listed = run(home, "roots", "ls")
    assert listed.stdout == f"p\t{w}/P\taway\n", listed.stdout
    pruned = run(home, "-n", "--yes", "prune", "--all")
    assert pruned.returncode == 0 and pruned.stdout == "", (pruned.stdout, pruned.stderr)
    assert "root p is declared and not here" in pruned.stderr, pruned.stderr


def check_r5_file_verbs_refuse_what_omafiles_keeps(data, workspace):
    """`rm`, `trash`, `mv` and `cp` refuse an operand that is, or holds, a file
    omafiles keeps, at either end, and touch nothing."""
    home, w = fresh(workspace, "r5-kept-files")
    (w / "x").write_text("x")
    assert run(home, "tag", "add", "-t", "t", "--", w / "x").returncode == 0
    assert run(home, "roots", "add", "--id", "r", w).returncode == 0
    assert run(home, "sweep").returncode == 0
    index = home / "omafiles" / "index.jsonl"
    before = index.read_bytes()
    kept = (index, home / "omafiles", home, home / "config" / "omafiles" / "roots.jsonl")
    stray = home / "state" / "omafiles" / "x"
    stray.write_text("x")
    for words in [("--yes", "rm", index), ("--yes", "rm", home / "config" / "omafiles" / "roots.jsonl"),
                  ("-n", "--yes", "trash", home / "omafiles"), ("-n", "--yes", "trash", home),
                  ("mv", index, w / "i"), ("cp", index, w / "i"),
                  ("cp", w / "x", str(index) + ".lock"),
                  ("mv", w / "x", home / "config" / "omafiles" / "views.jsonl"),
                  ("mv", w / "x", home / "cache" / "omafiles" / "names" / "r"),
                  ("mv", "--replace", "x", "--with", "sweep.json", "--", stray)]:
        refused = run(home, *words)
        assert refused.returncode == 1, (words, refused.returncode, refused.stderr)
        assert "which omafiles keeps" in refused.stderr or "omafiles keeps it" in refused.stderr, \
            (words, refused.stderr)
    assert all(path.exists() for path in kept) and index.read_bytes() == before
    assert (w / "x").exists() and not (w / "i").exists()
    moved = run(home, "mv", "--replace", "x", "--with", "sweep.json", "--", w / "x")
    assert moved.returncode == 0 and (w / "sweep.json").exists(), moved.stderr


def check_r5_walk_filters_name_what_they_cannot_read(data, workspace):
    """In the walk, a link whose kind or time cannot be read -- a loop -- is
    named and left out by `--type`, `--changed-within` and `--ext`, exit 1."""
    home, w = fresh(workspace, "r5-walk-shape")
    (w / "f.pdf").write_text("f")
    (w / "d.pdf").mkdir()
    (w / "loop.pdf").symlink_to("loop.pdf")
    (w / "dangling.pdf").symlink_to("nowhere")
    for flags, listed in ((("--type", "f"), ["dangling.pdf", "f.pdf"]), (("--type", "d"), ["d.pdf"]),
                          (("--changed-within", "1h"), ["d.pdf", "dangling.pdf", "f.pdf"]),
                          (("--ext", "pdf"), ["dangling.pdf", "f.pdf"])):
        answer = run(home, "ls", "--untagged", *flags, w)
        assert answer.returncode == 1, (flags, answer.returncode, answer.stderr)
        assert answer.stdout.splitlines() == [f"{w}/{name}" for name in listed], (flags, answer.stdout)
        assert f"{w}/loop.pdf: " in answer.stderr and "it was not listed" in answer.stderr, \
            (flags, answer.stderr)


def check_r5_a_list_of_names_holds_each_path_once(data, workspace):
    """A list line `.` or a line repeated is not what `sweep` writes, and the
    list is refused."""
    home, w = fresh(workspace, "r5-names-once")
    (w / "a").write_text("a")
    assert run(home, "roots", "add", "--id", "r", w).returncode == 0
    assert run(home, "sweep").returncode == 0
    names = home / "cache" / "omafiles" / "names" / "r"
    written = names.read_text()
    for extra, said in ((".\n", "is not a clean path"), ("a\n", "repeats a path listed above")):
        names.write_text(written + extra)
        refused = run(home, "ls", "--untagged", "--cached")
        assert refused.returncode == 1 and said in refused.stderr, refused.stderr
        assert refused.stdout == "", refused.stdout


def check_r5_an_empty_name_is_quoted(data, workspace):
    """An empty operand is spelled `''`, as coreutils spells it."""
    home, w = fresh(workspace, "r5-empty-name")
    (w / "x").write_text("x")
    for verb in ("json", "ls", "tags", "has"):
        words = (verb, "-t", "a", "") if verb == "has" else (verb, "")
        said = run(home, *words)
        assert said.returncode == 2, (verb, said.returncode, said.stderr)
        assert f"{verb}: path missing: ''" in said.stderr, (verb, said.stderr)
    for verb, doing in (("mv", "cannot move"), ("cp", "cannot copy")):
        said = run(home, verb, w / "x", "")
        assert said.returncode == 1, (verb, said.stderr)
        assert f"{verb}: {doing} {w}/x to '': No such file" in said.stderr, said.stderr
    said = run(home, "batch", "")
    assert said.returncode == 2 and "batch: cannot read '': " in said.stderr, said.stderr


def check_r5_an_empty_value_is_spelled_not_missing(data, workspace):
    """A flag or word given empty is named `''` in its refusal, and a flag the
    verb requires, given empty, is not reported missing."""
    home, w = fresh(workspace, "r5-empty-value")
    (w / "x").write_text("x")
    for words, said in ((("recent", ""), "recent: '' is not a count"),
                        (("tag", "cp", "--from", "", w / "x"), "tag cp: path missing: ''"),
                        (("view", "add", "--name", "", "-t", "a"), "view add: '' is not a name"),
                        (("view", "rm", "--name", ""), "view rm: no view called ''"),
                        (("roots", "add", "--id", "", w), "roots add: '' is not an id"),
                        (("roots", "rm", "--id", ""), "roots rm: '' is not declared"),
                        (("ls", "-t", ""), "invalid tag: '' ("),
                        (("ls", "--since", ""), "invalid duration: '' ("),
                        (("ls", "--type", ""), "not ''\n"),
                        (("chmod", "", w / "x"), "chmod: invalid mode: '';"),
                        (("completions", ""), "unknown shell '' ("),
                        (("view", ""), "view: unknown subcommand ''\n"),
                        (("tag", ""), "tag: unknown subcommand ''\n"),
                        (("",), "unknown command: '' (")):
        refused = run(home, *words)
        assert refused.returncode in (1, 2), (words, refused.returncode, refused.stderr)
        assert said in refused.stderr, (words, refused.stderr)
        assert "is required" not in refused.stderr, (words, refused.stderr)
    for words, said in ((("tag", "cp", w / "x"), "tag cp: --from is required"),
                        (("view", "rm"), "view rm: --name is required"),
                        (("roots", "add", w), "roots add: --id is required")):
        refused = run(home, *words)
        assert refused.returncode == 1 and said in refused.stderr, (words, refused.stderr)


def check_r5_a_dangling_destination_parent_is_named_hop_by_hop(data, workspace):
    """A destination whose folder part is a link that leads nowhere names each
    hop, as a source does."""
    home, w = fresh(workspace, "r5-dangling-destination")
    (w / "a").write_text("a")
    (w / "D").symlink_to("nowhere")
    (w / "D2").symlink_to("D")
    for verb in ("mv", "cp"):
        said = run(home, verb, w / "a", w / "D2" / "x")
        assert said.returncode == 1, (verb, said.stderr)
        assert (f"{w}/D2 is a symlink to {w}/D, which is a symlink to {w}/nowhere, "
                "which is not there") in said.stderr, said.stderr


def check_r5_the_prompt_keeps_each_name_on_one_line(data, workspace):
    """The list a removal asks about quotes each name, so a newline in one is
    not a second line."""
    home, w = fresh(workspace, "r5-prompt-quoted")
    odd = w / "n\nl"
    odd.write_text("x")
    refused = run(home, "rm", odd)
    assert refused.returncode == 1, refused.stderr
    assert f"'{w}/n'$'\\x0a''l'" in refused.stderr, refused.stderr
    assert "\nl\n" not in refused.stderr, refused.stderr


def check_r5_chmod_dry_run_follows_one_inode(data, workspace):
    """`chmod -n` over two hard links previews the modes the run prints."""
    home, w = fresh(workspace, "r5-chmod-links")
    (w / "a").write_text("a")
    (w / "a").chmod(0o600)
    os.link(w / "a", w / "b")
    planned = run(home, "-n", "chmod", "g+r,o+w", w / "a", w / "b")
    done = run(home, "chmod", "g+r,o+w", w / "a", w / "b")
    assert planned.returncode == 0 and done.returncode == 0, (planned.stderr, done.stderr)
    assert planned.stdout == done.stdout, (planned.stdout, done.stdout)
    assert done.stdout.splitlines()[1].startswith("0642\t0642\t"), done.stdout


def check_r5_cp_drops_the_sticky_bit(data, workspace):
    """A copy takes the source's mode less setuid, setgid and sticky, then the
    umask."""
    home, w = fresh(workspace, "r5-cp-sticky")
    source = w / "s"
    source.write_text("s")
    source.chmod(0o1777)
    done = run(home, "cp", source, w / "t", preexec_fn=lambda: os.umask(0o077))
    assert done.returncode == 0, done.stderr
    assert (w / "t").stat().st_mode & 0o7777 == 0o700, oct((w / "t").stat().st_mode)


def check_r5_roots_add_refuses_an_empty_check(data, workspace):
    """`roots add --check ''` is refused rather than declaring a root with no
    check, which the loader would then refuse."""
    home, w = fresh(workspace, "r5-empty-check")
    refused = run(home, "roots", "add", "--check", "", "--id", "r", w)
    assert refused.returncode == 1 \
        and refused.stderr.startswith("roots add: --check takes a path inside the root"), \
        refused.stderr
    assert run(home, "roots", "ls").stdout == ""


def check_r5_input_behind_an_unnameable_link_is_refused(data, workspace):
    """`batch` and `import` refuse a link to a name that is not valid UTF-8, as
    every operand is refused."""
    home, w = fresh(workspace, "r5-input-unnameable")
    target = os.path.join(os.fsencode(w), b"b\xff")
    with open(target, "wb"):
        pass
    os.symlink(target, os.fsencode(w / "lnb"))
    for verb in ("batch", "import"):
        said = run(home, verb, w / "lnb")
        assert said.returncode == 1, (verb, said.returncode, said.stderr)
        assert "which is not valid UTF-8" in said.stderr, (verb, said.stderr)


def check_r5_a_closed_stdin_is_not_read_by_name(data, workspace):
    """With stdin closed, a name that reaches it is refused as `-` is, and
    `/dev/null` itself still reads as empty."""
    home, w = fresh(workspace, "r5-closed-stdin-by-name")
    (w / "in").symlink_to("/dev/stdin")
    for words in (("batch", "/dev/stdin"), ("--json", "import", "/proc/self/fd/0"),
                  ("batch", "/dev/fd/0"), ("batch", w / "in")):
        done = run(home, *words, preexec_fn=lambda: os.close(0))
        assert done.returncode == 1, (words, done.returncode, done.stderr)
        assert "cannot read stdin: Bad file descriptor" in done.stderr, (words, done.stderr)
        assert done.stdout == "", (words, done.stdout)
    empty = run(home, "batch", "/dev/null", preexec_fn=lambda: os.close(0))
    assert empty.returncode == 0, empty.stderr


def check_r5_no_value_where_none_can_be_is_no_refusal(data, workspace):
    """A file on a filesystem with no `user.*` namespace holds no value, so
    `tag clear` and `tag rm` on one with no row refuse nothing: procfs answers
    ENOTSUP to the remove itself on `comm`, and EACCES to it, then ENOTSUP to
    the read, on the read-only `cmdline`."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        return
    home, w = fresh(workspace, "r5-no-namespace")
    (w / "p").mkdir()
    wrap = [bwrap, "--dev-bind", "/", "/", "--proc", str(w / "p")]
    for name in ("comm", "cmdline"):
        for words in (("tag", "clear"), ("tag", "rm", "-t", "k")):
            done = run(home, "-j", *words, "--", w / "p" / "self" / name, wrap=wrap)
            assert done.returncode == 0 and done.stdout == "[]\n" and done.stderr == "", \
                (name, words, done)


def check_r5_view_and_roots_subcommands_take_their_own(data, workspace):
    """Each `view` and `roots` subcommand takes its own flags and operands,
    from the table completion offers out of, and refuses the rest with
    nothing done."""
    home, w = fresh(workspace, "r5-subcommand-table")
    (w / "x").write_text("x")
    assert run(home, "tag", "add", "-t", "work", "--", w / "x").returncode == 0
    assert run(home, "roots", "add", "--id", "r", w).returncode == 0
    assert run(home, "view", "add", "--name", "v", "-t", "work").returncode == 0
    for words, said in ((("view", "rm", "--name", "v", "extra"), "view rm: takes no arguments"),
                        (("view", "rm", "--name", "v", "--", "other"), "view rm: takes no arguments"),
                        (("view", "ls", "--name", "foo"), "unexpected --name"),
                        (("view", "ls", "--tag", "x"), "unexpected --tag"),
                        (("view", "--name", "v"), "unexpected --name"),
                        (("view", "add", "--name", "z", w, w), "view add: takes at most one path"),
                        (("view", "frob"), "view: unknown subcommand frob"),
                        (("roots", "migrate", "x"), "roots migrate: takes no arguments"),
                        (("roots", "rm", "--id", "r", "/some/path"), "roots rm: takes no arguments"),
                        (("roots", "rm", "--id", "r", "--mount"), "unexpected --mount"),
                        (("roots", "ls", "--check", "q"), "unexpected --check"),
                        (("roots", "add", "--id", "q"), "roots add: takes one path"),
                        (("roots", "frob"), "roots: unknown subcommand frob"),
                        (("export", "x"), "export: takes no arguments")):
        for dry in ((), ("-n",)):
            refused = run(home, *dry, *words)
            assert refused.returncode == 1 and said in refused.stderr and refused.stdout == "", \
                (words, refused)
    assert run(home, "view", "ls").stdout.startswith("v\t")
    assert run(home, "roots", "ls").stdout.startswith("r\t")

    def offered(*words):
        done = run(home, "__complete", "--", *words)
        return done.returncode, done.stdout.split()

    assert offered("view", "") == (0, ["add", "ls", "rm"])
    assert offered("roots", "m") == (0, ["add", "ls", "migrate", "rm"])
    globals_only = ["--dry-run", "--help", "--json", "--quiet", "--version", "--yes"]
    for words, own in ((("view", "ls", "-"), []), (("view", "-"), []),
                       (("view", "rm", "-"), ["--name"]),
                       (("view", "add", "-"), ["--name", "--tag"]),
                       (("roots", "ls", "-"), []), (("roots", "migrate", "-"), []),
                       (("roots", "rm", "-"), ["--id"]),
                       (("roots", "add", "-"), ["--check", "--id", "--mount"])):
        assert offered(*words) == (0, sorted(globals_only + own)), (words, offered(*words))
    assert offered("view", "add", "--tag", "") == (0, ["work"])
    assert offered("view", "--tag", "")[0] == 1


def check_r5_the_bash_shim_completes_after_an_equals(data, workspace):
    """Bash splits `--tag=` at the `=`, which is in COMP_WORDBREAKS, so the
    value being typed is empty, as git-completion reassembles it."""
    bash = shutil.which("bash")
    if not bash:
        return
    home, w = fresh(workspace, "r5-bash-equals")
    (w / "x").write_text("x")
    assert run(home, "tag", "add", "-t", "work", "--", w / "x").returncode == 0
    script = run(home, "completions", "bash").stdout
    probe = script + 'omafiles() { "$OMAFILES" "$@"; }\n' + "".join(
        f"COMP_WORDS=({words}); COMP_CWORD={cword}; _omafiles omafiles; "
        'printf "[%s]\\n" "${COMPREPLY[*]}"\n'
        for words, cword in (("omafiles tag add --tag =", 4),
                             ("omafiles tag add --tag = wo", 5),
                             ("omafiles ls --type =", 3)))
    done = subprocess.run([bash, "-c", probe], env={**cli_env(home), "OMAFILES": str(CLI)},
                          cwd=str(w), text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
    assert done.stdout.splitlines() == ["[work]", "[work]", "[dir file]"], done


def check_r5_a_flush_writes_only_what_differs(data, workspace):
    """A value already holding what would be written is not written again, and
    one whose valid tags already are the row's refuses nothing where no
    `reindex` walked it."""
    home, w = fresh(workspace, "r5-flush-no-op")
    if not carries_xattrs(w):
        return
    g = w / "g"
    g.write_text("g")
    os.setxattr(g, "user.omafiles.tags", b"Bad W")
    for dry in ((), ("-n",)):
        done = run(home, *dry, "tag", "rm", "-t", "zz", "--", g)
        assert done.returncode == 0 and done.stderr == "", (dry, done)
    assert os.getxattr(g, "user.omafiles.tags") == b"Bad W"
    walked = run(home, "reindex", w)
    assert walked.returncode == 1 and "tag clear" in walked.stderr, walked
    # A tree `reindex` walked still disagrees while the words beside the tags
    # are there, whatever the row holds.
    (w / "t").mkdir()
    ok = w / "t" / "ok"
    ok.write_text("o")
    os.setxattr(ok, "user.omafiles.tags", b"ok,Bad W")
    assert run(home, "tag", "add", "-t", "ok", "--", ok).returncode == 0
    again = run(home, "reindex", w / "t")
    assert again.returncode == 1 and "left as it is" in again.stderr, again
    if os.geteuid() == 0:
        return
    r = w / "r"
    r.write_text("r")
    assert run(home, "tag", "add", "-t", "a", "--", r).returncode == 0
    r.chmod(0o444)
    (w / "d.jsonl").write_text(run(home, "export").stdout)
    imported = run(home, "import", w / "d.jsonl")
    assert imported.returncode == 0 and imported.stderr == "", imported
    added = run(home, "tag", "add", "-t", "a", "--", r)
    assert added.returncode == 0 and added.stderr == "", added


def check_r5_the_tag_verbs_keep_their_per_file_rules(data, workspace):
    """`tag merge` renames a row whose file's value will not read and says the
    copy it skipped; `tag cp` refuses a hard link to `--from` as the same file;
    a batch that takes a tag away and puts it back keeps the row's stamp."""
    home, w = fresh(workspace, "r5-tag-rules")
    a = w / "a"
    a.write_text("a")
    write_index(home, [{"path": str(a), "tags": ["x", "y"], "tagged": 1000}])
    batched = run(home, "batch", stdin=f"tag rm --tag x -- {a}\ntag add --tag x -- {a}\n")
    assert batched.returncode == 0, batched
    assert json.loads(run(home, "json", "--", a).stdout)["tagged"] == 1000
    src = w / "src"
    src.write_text("s")
    os.link(src, w / "hl")
    assert run(home, "tag", "add", "-t", "t", "--", src).returncode == 0
    same = run(home, "tag", "cp", "--from", src, "--", w / "hl")
    assert same.returncode == 1 and "is the file the tags come from" in same.stderr, same
    assert run(home, "json", "--", w / "hl").stdout == ""
    if os.geteuid() == 0:
        return
    m, n = w / "m", w / "n"
    for path in (m, n):
        path.write_text(path.name)
    assert run(home, "tag", "add", "-t", "old", "--", m, n).returncode == 0
    m.chmod(0o000)
    try:
        merged = run(home, "tag", "merge", "old", "new")
    finally:
        m.chmod(0o644)
    assert merged.returncode == 0, merged
    assert f"{m}" in merged.stderr and "its xattr was not written" in merged.stderr, merged
    for path in (m, n):
        assert run(home, "tag", "ls", "--", path).stdout.split() == ["new"], path


def check_r5_completion_offers_what_the_parse_takes(data, workspace):
    """`--` before a verb takes nothing; a flag already said is not offered
    again, except the lists; `--cached` goes with `--name` alone; a tag
    position with no tags is an empty answer and not a path; a broken roots
    file costs only the tags of rows under a root."""
    home, w = fresh(workspace, "r5-completion")

    def offered(*words):
        done = run(home, "__complete", "--", *words)
        return done.returncode, done.stdout.split()

    assert offered("--", "") == (0, [])
    assert offered("tag", "add", "--tag", "") == (0, [])
    _, flags = offered("ls", "--type", "d", "-")
    assert "--type" not in flags and "--tag" in flags, flags
    _, flags = offered("ls", "--untagged", "-")
    assert "--untagged" not in flags and "--cached" in flags, flags
    _, flags = offered("ls", "--name", "x", "-")
    assert "--name" not in flags and "--cached" in flags, flags
    _, flags = offered("ls", "--name", "x", "--tag", "t", "-")
    assert "--cached" not in flags and "--tag" in flags, flags
    _, flags = offered("recent", "--desktop", "-")
    assert "--desktop" not in flags, flags
    (w / "r").mkdir()
    (w / "loose").write_text("l")
    assert run(home, "roots", "add", "--id", "r", w / "r").returncode == 0
    (w / "r" / "in").write_text("i")
    assert run(home, "tag", "add", "-t", "inside", "--", w / "r" / "in").returncode == 0
    assert run(home, "tag", "add", "-t", "outside", "--", w / "loose").returncode == 0
    (home / "config" / "omafiles" / "roots.jsonl").write_text("broken\n")
    assert offered("tag", "add", "--tag", "") == (0, ["outside"])


def check_r5_a_value_is_spelled_once(data, workspace):
    """A byte that is not UTF-8 and the text that spells one print apart, in a
    note and in `verify`; an empty word is named as one; `reindex` names its
    verb; `import` says a mirror it did not attempt once per path."""
    home, w = fresh(workspace, "r5-spelled-once")
    if not carries_xattrs(w):
        return
    byte, text = w / "byte", w / "text"
    for path, value in ((byte, b"b\xff"), (text, b"b\\xff")):
        path.write_text(path.name)
        os.setxattr(path, "user.omafiles.tags", value)
    checked = run_bytes(home, "verify", w).stdout
    assert b"byte\txattr=b\\xff;" in checked and b"text\txattr=b\\\\xff;" in checked, checked
    for path, said in ((byte, b"(b\\xff)"), (text, b"(b\\\\xff)")):
        noted = run_bytes(home, "tag", "add", "-t", "k", "--", path)
        assert noted.returncode == 0 and said in noted.stderr, (path, noted.stderr)
    comma = w / "comma"
    comma.write_text("c")
    os.setxattr(comma, "user.omafiles.tags", b",")
    noted = run(home, "tag", "add", "-t", "k", "--", comma)
    assert "(an empty word)" in noted.stderr, noted.stderr
    if os.geteuid() == 0:
        return
    shut = w / "shut"
    shut.write_text("s")
    shut.chmod(0o000)
    try:
        walked = run(home, "reindex", w)
    finally:
        shut.chmod(0o644)
    assert f"reindex: xattr get failed on {shut}" in walked.stderr, walked.stderr
    (w / "closed").mkdir()
    inner = w / "closed" / "f"
    inner.write_text("f")
    (w / "dump").write_text("".join(json.dumps({"path": str(inner), "tags": [t]}) + "\n"
                                    for t in ("p", "q")))
    (w / "closed").chmod(0o000)
    try:
        imported = run(home, "import", w / "dump")
    finally:
        (w / "closed").chmod(0o755)
    assert imported.stderr.count("was not written") == 1, imported.stderr


def check_r5_a_kept_file_that_will_not_write_says_why(data, workspace):
    """The views, the roots, the receipt, a root's list of names and the lock
    carry the system's reason, not Qt's summary of it."""
    home, w = fresh(workspace, "r5-kept-why")
    state = home / "state" / "omafiles"
    (state / "sweep.json").mkdir(parents=True)
    assert run(home, "roots", "add", "--id", "r", w).returncode == 0
    (home / "cache" / "omafiles" / "names" / "r").mkdir(parents=True)
    swept = run(home, "sweep")
    assert "sweep.json: Is a directory" in swept.stderr, swept.stderr
    assert "names/r: Is a directory" in swept.stderr, swept.stderr
    bwrap = shutil.which("bwrap")
    if bwrap:
        # A data directory on a one-page tmpfs, filled before the run.
        (home / "omafiles").mkdir(exist_ok=True)
        full = run(home, "tag", "add", "-t", "t", "--", w, wrap=[
            bwrap, "--dev-bind", "/", "/", "--size", "4096", "--tmpfs", str(home / "omafiles"),
            "sh", "-c", 'head -c 65536 /dev/zero > "$0/fill" 2>/dev/null; exec "$@"',
            str(home / "omafiles")])
        assert full.returncode == 3 and "cannot lock" in full.stderr \
            and "No space left on device" in full.stderr, full
    if os.geteuid() == 0:
        return
    config = home / "config" / "omafiles"
    (w / "sub").mkdir()
    config.chmod(0o555)
    try:
        for words in (("view", "add", "--name", "v", w), ("roots", "add", "--id", "s", w / "sub")):
            said = run(home, *words)
            assert said.returncode == 3 and said.stderr.endswith(": Permission denied\n"), said
    finally:
        config.chmod(0o755)


def check_r5_help_names_no_file_it_does_not_ship(data, workspace):
    """`--help` does not point at a reference an installed copy lacks."""
    for words in (("--help",), ("tag", "--help"), ("completions", "--help")):
        assert "docs/cli.md" not in run(data, *words).stdout, words


def in_mounts(home, setup, script):
    """`script`, a bash body, run in a user and mount namespace after `setup`
    mounts what it names, as the calling user again, with `$CLI` the binary and
    the sandbox's environment. None where the namespace or a mount cannot be
    made here."""
    unshare = shutil.which("unshare")
    if not unshare:
        return None
    body = (f"{setup} || exit 97; "
            f'exec {unshare} -U --map-user={os.getuid()} bash -c "$0"')
    done = subprocess.run([unshare, "-rm", "sh", "-c", body, script],
                          env={**cli_env(home), "CLI": str(CLI), "LC_ALL": "C"},
                          cwd=str(ROOT), text=True, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return None if done.returncode == 97 else done


def check_r6a_a_batch_reads_a_hard_links_pending_copy(data, workspace):
    """Two names of one file in one batch end as the same lines run one by
    one: a later line reads the copy an earlier one queued under the other
    name, and the file ends with the copy of the line written last."""
    lines = (("tag add --tag s1 -- k1\ntag add --tag s3 -- k2\n", b"s1,s3"),
             ("tag add --tag a -- k1\ntag add --tag b -- k2\ntag add --tag c -- k1\n", b"a,b,c"))
    for number, (body, value) in enumerate(lines):
        answers = []
        for batched in (True, False):
            home, w = fresh(workspace, f"r6a-hard-links-{number}-{batched}")
            if not carries_xattrs(w):
                return
            (w / "k1").write_text("h")
            os.link(w / "k1", w / "k2")
            if batched:
                assert run(home, "batch", "-", stdin=body, cwd=w).returncode == 0
            else:
                for line in body.splitlines():
                    assert run(home, *line.split(), cwd=w).returncode == 0, line
            rows = {Path(row["path"]).name: row["tags"]
                    for row in map(json.loads, run(home, "export").stdout.splitlines())}
            answers.append((os.getxattr(w / "k1", "user.omafiles.tags"), rows))
        assert answers[0] == answers[1] and answers[0][0] == value, (body, answers)


def check_r6a_a_folded_spelling_is_the_stored_name(data, workspace):
    """On a directory that folds case, an operand spelled another way is the
    name the directory stores, so its rows move, go and copy with it; and a
    change of case alone is a rename, of a file and of a folder, rows and all."""
    home, w = fresh(workspace, "r6a-casefold")
    cf = w / "cf"
    cf.mkdir()
    done = in_mounts(home, f"mount -t tmpfs -o casefold tmpfs {cf} && chattr +F {cf}", f"""
cd {cf}
touch Photo.JPG B.txt C.txt IMG.JPG; mkdir Sub Dir; touch Sub/f Dir/in
"$CLI" tag add -t x Photo.JPG B.txt C.txt Sub Sub/f IMG.JPG Dir/in >/dev/null
"$CLI" mv photo.jpg moved.jpg >/dev/null && echo moved
"$CLI" -y rm b.txt >/dev/null && echo removed
"$CLI" cp c.txt c2.txt >/dev/null && echo copied
"$CLI" mv sub Sub2 >/dev/null && echo folder
"$CLI" mv IMG.JPG img.jpg && echo recased
"$CLI" mv Dir dir && echo recased
"$CLI" mv img.jpg img.jpg || echo same
"$CLI" verify . && echo verified
ls
"$CLI" export
ln -s C.txt Link
"$CLI" -y rm link
"$CLI" -n tag add -t q c.txt C.TXT
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    assert lines[:10] == ["moved", "removed", "copied", "folder", f"{cf}/img.jpg", "recased",
                          f"{cf}/dir", "recased", "same", "verified"], done
    assert lines[10:16] == ["C.txt", "Sub2", "c2.txt", "dir", "img.jpg", "moved.jpg"], done
    rows = sorted(json.loads(line)["path"] for line in lines[16:-2])
    assert rows == [f"{cf}/{name}" for name in
                    ("C.txt", "Sub2", "Sub2/f", "c2.txt", "dir/in", "img.jpg", "moved.jpg")], done
    assert lines[-2:] == [f"{cf}/Link", f"{cf}/C.txt"], done


def check_r6a_a_filesystem_without_user_xattrs_holds_only_rows(data, workspace):
    """Where a filesystem has no `user.*` namespace the row is the only copy:
    tagging says nothing about it, `verify` finds nothing, `reindex` either way
    and `sweep` do not try, a copy off it carries the row's tags, and a copy onto it of a
    value no row records is still refused."""
    home, w = fresh(workspace, "r6a-no-namespace")
    if not carries_xattrs(w):
        return
    rf = w / "rf"
    rf.mkdir()
    (w / "loose").write_text("l")
    os.setxattr(w / "loose", "user.omafiles.tags", b"solo")
    (w / "rowed").write_text("r")
    assert run(home, "tag", "add", "-t", "kept", "--", w / "rowed").returncode == 0
    done = in_mounts(home, f"mount -t ramfs ramfs {rf}", f"""
cd {rf}
echo a > a.txt
"$CLI" tag add -t work -- a.txt 2>&1; echo "add $?"
"$CLI" verify . 2>&1; echo "verify $?"
"$CLI" reindex . 2>&1; echo "reindex $?"
"$CLI" reindex --from-xattr . 2>&1; echo "from $?"
"$CLI" sweep . 2>&1 >/dev/null; echo "sweep $?"
"$CLI" cp a.txt {w}/copied 2>&1 >/dev/null; echo "off $?"
"$CLI" cp {w}/loose loose 2>&1 >/dev/null; echo "onto $?"
"$CLI" cp {w}/rowed rowed 2>&1 >/dev/null; echo "rowed $?"
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    assert lines[:7] == ["add 0", "verify 0", "reindex 0", "from 0",
                         "sweep: adopted 0, merged 0, gone 0, unreadable 0, skipped 0, listed 0, "
                         "unlistable 0", "sweep 0", "off 0"], done
    assert lines[7].startswith("cp: cannot copy") and "which no row records" in lines[7], done
    assert lines[8:] == ["onto 1", "rowed 0"], done
    assert os.getxattr(w / "copied", "user.omafiles.tags") == b"work"
    assert run(home, "tag", "ls", "--", w / "copied").stdout == "work\n"


def check_r6a_a_bind_mount_is_another_filesystem_to_mv(data, workspace):
    """`rename(2)` answers EXDEV between two mounts of one filesystem, so a
    folder or a fifo moved across a bind mount is refused as across any
    filesystem, in a dry run as for real; a file crosses."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        return
    home, w = fresh(workspace, "r6a-bind-mount")
    (w / "src" / "d").mkdir(parents=True)
    (w / "mnt").mkdir()
    os.mkfifo(w / "src" / "ff")
    (w / "src" / "f").write_text("f")
    wrap = [bwrap, "--dev-bind", "/", "/", "--bind", str(w / "src"), str(w / "mnt")]
    for dry in (("-n",), ()):
        folder = run(home, *dry, "mv", w / "src/d", w / "mnt/d2", wrap=wrap)
        assert folder.returncode == 1 and "a folder cannot cross" in folder.stderr, folder
        fifo = run(home, *dry, "mv", w / "src/ff", w / "mnt/ff2", wrap=wrap)
        assert fifo.returncode == 1 and "is not a regular file" in fifo.stderr, fifo
    assert run(home, "mv", w / "src/f", w / "mnt/f2", wrap=wrap).returncode == 0
    assert sorted(os.listdir(w / "src")) == ["d", "f2", "ff"]


def check_r6a_a_kept_file_is_known_by_its_inode(data, workspace):
    """A file omafiles keeps is refused by any name: a hard link to the index,
    and the declarations through a bind mount of their directory."""
    home, w = fresh(workspace, "r6a-kept-inode")
    (w / "x").write_text("x")
    assert run(home, "tag", "add", "-t", "t", "--", w / "x").returncode == 0
    assert run(home, "roots", "add", "--id", "r", w).returncode == 0
    os.link(home / "omafiles" / "index.jsonl", w / "hard")
    refused = run(home, "-y", "rm", w / "hard")
    assert refused.returncode == 1 and "omafiles keeps it" in refused.stderr, refused
    assert (w / "hard").exists()
    bwrap = shutil.which("bwrap")
    if not bwrap:
        return
    (w / "alias").mkdir()
    wrap = [bwrap, "--dev-bind", "/", "/", "--bind", str(home / "config"), str(w / "alias")]
    for words in (("-y", "rm", w / "alias/omafiles/roots.jsonl"),
                  ("mv", w / "alias/omafiles/roots.jsonl", w / "r"),
                  ("mv", w / "x", w / "alias/omafiles/views.jsonl")):
        refused = run(home, *words, wrap=wrap)
        assert refused.returncode == 1 and "omafiles keeps it" in refused.stderr, (words, refused)
    held = run(home, "-n", "-y", "trash", w / "alias", wrap=wrap)
    assert held.returncode == 1 and "which omafiles keeps" in held.stderr, held
    assert (home / "config" / "omafiles" / "roots.jsonl").exists()


def check_r6a_mv_leaves_a_declared_root_where_it_is(data, workspace):
    """`mv` refuses the directory of a root that is here, and one holding it,
    alone or in a set, naming the root and the command that moves it."""
    home, w = fresh(workspace, "r6a-root-mv")
    (w / "top" / "inner").mkdir(parents=True)
    assert run(home, "roots", "add", "--id", "inner", w / "top/inner").returncode == 0
    for words, said in ((("mv", w / "top/inner", w / "top/moved"), "it is root inner"),
                        (("-n", "mv", w / "top", w / "top2"),
                         f"it holds root inner at {w}/top/inner"),
                        (("mv", "--replace", "inner", "--with", "x", "--", w / "top/inner"),
                         "it is root inner")):
        refused = run(home, *words)
        assert refused.returncode == 1 and said in refused.stderr \
            and "`omafiles roots rm --id inner` first" in refused.stderr, (words, refused)
    assert (w / "top" / "inner").is_dir()


def check_r6a_a_destination_drops_the_rows_under_it(data, workspace):
    """A file landing where rows sit under its name drops them, as a folder
    landing there does: their files are gone."""
    home, w = fresh(workspace, "r6a-stale-under")
    for verb in ("cp", "mv"):
        (w / "d").mkdir()
        (w / "d" / "y").write_text("y")
        (w / "s").write_text("s")
        assert run(home, "tag", "add", "-t", "old", "--", w / "d/y").returncode == 0
        shutil.rmtree(w / "d")
        landed = run(home, verb, w / "s", w / "d")
        assert landed.returncode == 0, landed
        assert f"{w}/d/y had a row for a file that is gone" in landed.stderr, landed
        assert "d/y" not in run(home, "export").stdout
        os.unlink(w / "d")
        (w / "s").unlink(missing_ok=True)
    (w / "a").write_text("a")
    (w / "c").mkdir()
    (w / "c" / "z").write_text("z")
    assert run(home, "tag", "add", "-t", "old", "--", w / "c/z").returncode == 0
    shutil.rmtree(w / "c")
    renamed = run(home, "mv", "--pattern", "c", "--", w / "a")
    assert renamed.returncode == 0, renamed
    assert f"{w}/c/z had a row for a file that is gone" in renamed.stderr, renamed
    assert "c/z" not in run(home, "export").stdout


def check_r6a_a_move_the_index_would_not_take_is_undone(data, workspace):
    """Where the index cannot be written after the file moved, `mv` moves it
    back and `cp` removes its copy, exit 3; `rm` cannot, and says the rows stay
    for `prune`."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        return
    home, w = fresh(workspace, "r6a-commit-undo")
    for name in ("a", "b", "c"):
        (w / name).write_text(name)
    (w / "s").mkdir()
    (w / "s" / "x1").write_text("1")
    (w / "s" / "x2").write_text("2")
    assert run(home, "tag", "add", "-t", "t", "--", w / "a", w / "b", w / "c",
               w / "s/x1", w / "s/x2").returncode == 0
    assert run(home, "tag", "add", "-t", "incoming", "--", w / "b").returncode == 0
    kept = home / "omafiles"
    saved = w.parent / "index.saved"
    shutil.copy(kept / "index.jsonl", saved)
    # Two pages: the index and the lock fill them, so the commit's new copy fails.
    wrap = [bwrap, "--dev-bind", "/", "/", "--size", "8192", "--tmpfs", str(kept),
            "sh", "-c", 'cp "$1" "$0/index.jsonl"; shift; exec "$@"', str(kept), str(saved)]
    moved = run(home, "mv", w / "a", w / "a2", wrap=wrap)
    assert moved.returncode == 3 and "No space left on device" in moved.stderr, moved
    assert f"{w}/a2 was moved back to {w}/a" in moved.stderr and moved.stdout == "", moved
    copied = run(home, "cp", w / "b", w / "b2", wrap=wrap)
    assert copied.returncode == 3 and f"{w}/b2 was removed again" in copied.stderr, copied
    for verb, source, target in (("mv", w / "a", w / "b"), ("cp", w / "b", w / "a")):
        before = (source.read_bytes(), target.read_bytes())
        replacement = run(home, verb, "--force", "-T", "--", source, target, wrap=wrap)
        assert replacement.returncode == 3 and not replacement.stdout, replacement
        assert (source.read_bytes(), target.read_bytes()) == before
        assert not list(w.glob(".omafiles-replaced-*"))
    renamed = run(home, "mv", "--replace", "x", "--with", "y", "--", w / "s/x1", w / "s/x2",
                  wrap=wrap)
    assert renamed.returncode == 3 and renamed.stdout == "", renamed
    removed = run(home, "-y", "rm", w / "c", wrap=wrap)
    assert removed.returncode == 3 and "`omafiles prune` drops them" in removed.stderr, removed
    assert sorted(os.listdir(w)) == ["a", "b", "s"] and sorted(os.listdir(w / "s")) == ["x1", "x2"]


def check_r6a_a_drop_the_flush_did_not_make_has_no_receipt(data, workspace):
    """A path with no row whose value holds only words that are not tags is
    left as it is by `tag rm`, so no receipt names it."""
    home, w = fresh(workspace, "r6a-no-drop-receipt")
    if not carries_xattrs(w):
        return
    (w / "bw").write_text("b")
    os.setxattr(w / "bw", "user.omafiles.tags", b"Bad W")
    for words in (("--json", "tag", "rm", "-t", "zz", w / "bw"),
                  ("-n", "tag", "rm", "-t", "zz", w / "bw")):
        done = run(home, *words)
        assert done.returncode == 0 and done.stdout in ("", "[]\n"), (words, done)
    assert os.getxattr(w / "bw", "user.omafiles.tags") == b"Bad W"


def check_r6a_an_empty_batch_writes_nothing(data, workspace):
    """A batch with no line to run neither locks nor makes the index."""
    home, w = fresh(workspace, "r6a-empty-batch")
    for body in ("", "# only a comment\n\n"):
        done = run(home, "--json", "batch", "-", stdin=body)
        assert done.returncode == 0 and done.stdout == "[]\n", done
        assert not (home / "omafiles").exists()


def check_r6a_import_names_a_loop_of_links(data, workspace):
    """A dump path the kernel answers ELOOP for is not a path that is not here:
    its row is merged without its file's copy, and that is said."""
    home, w = fresh(workspace, "r6a-import-loop")
    (w / "l1").symlink_to("l2")
    (w / "l2").symlink_to("l1")
    dump = w / "dump"
    dump.write_text(json.dumps({"path": str(w / "l1"), "tags": ["x"]}) + "\n")
    done = run(home, "import", dump)
    assert done.returncode == 0, done
    assert f"import: {w}/l1: Too many levels of symbolic links; its own copy" in done.stderr, done


def check_r6_completion_reads_the_parse(data, workspace):
    """Completion offers only what the parse takes, from the parse's own
    tables: which flags repeat, which two refuse each other, how many operands
    and of what kind. A position that takes nothing, or a word, is an empty
    answer and not the shell's file names."""
    home, w = fresh(workspace, "r6-completion")
    (w / "f").write_text("f")
    assert run(home, "tag", "add", "-t", "work", "--", w / "f").returncode == 0

    def offered(*words):
        done = run(home, "__complete", "--", *words, cwd=w)
        return done.returncode, done.stdout.split()

    twice = run(home, "has", "--tag", "work", "--tag", "x", "--", w / "f")
    assert twice.returncode == 1 and "has: --tag was given twice" in twice.stderr, twice.stderr
    _, flags = offered("has", "--tag", "work", "-")
    assert "--tag" not in flags and "--quiet" in flags, flags
    _, flags = offered("ls", "--tag", "work", "-")
    assert "--tag" in flags, flags
    for words in (("ls", "--cached"), ("ls", "--untagged", "--cached"),
                  ("ls", "--name", "x", "--cached")):
        _, flags = offered(*words, "-")
        assert not {"--tag", "--any", "--since", "--cached"} & set(flags), (words, flags)
        assert "--ext" in flags, (words, flags)
        refused = run(home, *words, "--any", "work", cwd=w)
        assert refused.returncode == 1 and refused.stdout == "", (words, refused)
    for words in (("tag", "merge", "a", "b", ""), ("tag", "ls", "z", ""),
                  ("has", "--tag", "work", "f", ""), ("export", ""), ("json", "f", ""),
                  ("completions", "bash", ""), ("prune", "--all", ""), ("sweep", "--status", ""),
                  ("view", "add", "--name", "v", "--tag", "work", ""), ("tag", "ls", "--", "z", ""),
                  ("chmod", ""), ("recent", ""), ("ls", "--since", ""), ("ls", "--name", ""),
                  ("--json", "completions", ""), ("-j", "completions", "")):
        assert offered(*words) == (0, []), (words, offered(*words))
    assert offered("chmod", "644", "")[0] == 1
    assert offered("completions", "") == (0, ["bash", "zsh", "fish"])
    assert offered("tag", "merge", "") == (0, ["work"])
    assert offered("tag", "merge", "--tag", "") == (1, [])
    assert "completions" in offered("")[1]
    for spelled in ("-j", "--json"):
        assert "completions" not in offered(spelled, "")[1], spelled

    # Every flag offered to a verb is one its parse takes.
    for words in (("ls",), ("tags",), ("has",), ("json",), ("recent",), ("open",), ("mv",),
                  ("cp",), ("trash",), ("rm",), ("chmod",), ("prune",), ("reindex",),
                  ("sweep",), ("verify",), ("export",), ("import",), ("batch",),
                  ("completions",), ("tag", "add"), ("tag", "cp"), ("tag", "merge"),
                  ("view", "add"), ("view", "rm"), ("roots", "add"), ("roots", "rm")):
        for flag in offered(*words, "-")[1]:
            if flag in ("--help", "--version", "--json", "--quiet", "--yes", "--dry-run"):
                continue
            said = run(home, "-n", *words, flag, "value", cwd=w, stdin="")
            assert "unexpected" not in said.stderr and "unknown flag" not in said.stderr, \
                (words, flag, said.stderr)

    # A lone `-` is an operand, and a refusal of the parse names its verb.
    many = run(home, "batch", "-", "-", stdin="")
    assert many.returncode == 1 and "batch: one file at most" in many.stderr, many.stderr
    unknown = run(home, "ls", "-x", cwd=w)
    assert unknown.returncode == 1 and unknown.stderr.startswith("ls: unknown flag: -x"), unknown


def check_r6_bash_suffixes_only_paths(data, workspace):
    """A tag named like a folder in the current directory completes without a
    `/`: filename treatment is turned on only where the shim falls back to
    file names, as bash-completion's `_filedir` does."""
    bash = shutil.which("bash")
    if not bash:
        return
    home, w = fresh(workspace, "r6-bash-filenames")
    (w / "work").mkdir()
    (w / "f").write_text("f")
    assert run(home, "tag", "add", "-t", "work", "--", w / "f").returncode == 0
    (w / "c.bash").write_text(run(home, "completions", "bash").stdout)

    def tabbed(line):
        parent, child = pty.openpty()
        shell = subprocess.Popen(
            [bash, "--norc", "--noprofile", "-i"], stdin=child, stdout=child, stderr=child,
            cwd=str(w), env={**cli_env(home), "OMAFILES": str(CLI), "PS1": "$ ",
                             "TERM": "dumb", "INPUTRC": "/dev/null"})
        os.close(child)
        seen = b""

        def read_until(token, seconds):
            nonlocal seen
            end = time.time() + seconds
            while token not in seen and time.time() < end:
                if select.select([parent], [], [], 0.1)[0]:
                    try:
                        seen += os.read(parent, 4096)
                    except OSError:
                        break
        try:
            os.write(parent, b'omafiles() { "$OMAFILES" "$@"; }; . ./c.bash; '
                             b'complete -p omafiles; echo READY\n')
            read_until(b"READY\r\n$ ", 10)
            registered = seen
            seen = b""
            os.write(parent, line.encode() + b"\t")
            read_until(b"\x00", 2)
            return registered, seen
        finally:
            shell.kill()
            shell.wait()
            os.close(parent)

    registered, tag = tabbed("omafiles ls --tag wo")
    assert b"complete -F _omafiles omafiles" in registered, registered
    assert tag.endswith(b"--tag work "), tag
    _, path = tabbed("omafiles ls wo")
    assert path.endswith(b"ls work/"), path


def check_r6_zsh_script_runs_when_autoloaded(data, workspace):
    """On `$fpath` the file is the body of `_omafiles`, so the first Tab has to
    run what it defines; sourced, it registers. Cobra's guard."""
    zsh = run(data, "completions", "zsh").stdout
    assert zsh.startswith("#compdef omafiles\n"), zsh
    assert zsh.rstrip().endswith('if [ "$funcstack[1]" = "_omafiles" ]; then\n'
                                 '    _omafiles "$@"\nelse\n    compdef _omafiles omafiles\nfi'), zsh


def check_r6_a_refusal_spells_what_was_typed(data, workspace):
    """What was typed is named on one line: empty as `''`, a space at an end
    between quotes, a control -- C0 or C1, CR and NEL at an end too -- as
    `\\xHH` and unquoted; and a NUL in a value
    is `\\x00` under `--json` as in text."""
    home, w = fresh(workspace, "r6-spelled")
    f = w / "f"
    f.write_text("f")
    for words, said in ((("ls", "--name", ""), "ls: --name '' needs a pattern"),
                        (("recent", "1 "), "recent: '1 ' is not a count"),
                        (("has", "--tag", " ", "--", f), "has: invalid tag: ' ' ("),
                        (("tag", "add", "-t", "x\r", "--", f), "invalid tag: x\\x0d ("),
                        (("tag", "add", "-t", "x\ny", "--", f), "invalid tag: x\\ny ("),
                        (("tag", "add", "-t", "x\u0085y", "--", f), "invalid tag: x\\xc2\\x85y ("),
                        (("tag", "add", "-t", "x", "--", "nope\r"), "path missing: nope\\x0d\n"),
                        (("ls", "-\u0085"), "ls: unknown flag: -\\xc2\\x85\n"),
                        (("tag", "merge", "a\tb", "a\tb"), "invalid tag: a\\tb")):
        refused = run(home, *words, cwd=w)
        assert refused.returncode in (1, 2), (words, refused)
        assert said in refused.stderr, (words, refused.stderr)
        assert refused.stderr.count("\n") == 1 and "\r" not in refused.stderr, refused.stderr
    nel = w / "\u0085nel"
    nel.write_text("n")
    planned = run(home, "open", "--dry-run", nel)
    assert "'$'\\xc2\\x85''nel'" in planned.stdout and "\u0085" not in planned.stdout, planned
    if carries_xattrs(w):
        os.setxattr(f, "user.omafiles.tags", b"a\x00bc")
        problems = json.loads(run(home, "--json", "verify", w).stdout)["problems"]
        details = [problem["detail"] for problem in problems if problem["path"] == str(f)]
        assert details and all("a\\x00bc" in detail for detail in details), problems


def check_r6_a_cached_answer_looks_at_nothing(data, workspace):
    """`--cached` takes a row as the index holds it: a tagged file deleted since
    the sweep is not untagged. On a walk a row counts unless the path is now
    some other file."""
    home, w = fresh(workspace, "r6-cached-row")
    (w / "RR" / "c").mkdir(parents=True)
    for name in ("kept", "tagged"):
        (w / "RR" / "c" / name).write_text(name)
    assert run(home, "roots", "add", "--id", "rr", w / "RR").returncode == 0
    assert run(home, "tag", "add", "-t", "keep", "--", w / "RR/c/tagged").returncode == 0
    assert run(home, "-q", "sweep", w / "RR").returncode == 0
    (w / "RR" / "c" / "tagged").unlink()
    untagged = run(home, "-q", "ls", "--untagged", "--cached", w / "RR/c")
    assert untagged.stdout.splitlines() == [str(w / "RR/c/kept")], untagged
    named = json.loads(run(home, "-q", "--json", "ls", "--name", "tagged", "--cached",
                           w / "RR").stdout)
    assert [row["tags"] for row in named] == [["keep"]], named
    (w / "RR" / "c" / "tagged").symlink_to("kept")
    walked = run(home, "ls", "--untagged", w / "RR/c")
    assert str(w / "RR/c/tagged") in walked.stdout.splitlines(), walked


def check_r6_an_index_answer_asks_search_alone(data, workspace):
    """`ls`, `tags` and `prune` answer out of the index and stat the rows
    under `[dir]`; they never list it, so a directory with `x` and no `r`
    answers whole, and one nobody may search is said."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r6-search-only")
    rx = w / "rx"
    rx.mkdir()
    (rx / "f").write_text("f")
    assert run(home, "tag", "add", "-t", "rx", "--", rx / "f").returncode == 0
    for mode, whole in ((0o311, True), (0o600, False)):
        rx.chmod(mode)
        try:
            for words in (("ls", rx), ("tags", rx), ("-y", "-n", "prune", rx)):
                done = run(home, *words)
                assert (done.returncode == 0) == whole, (oct(mode), words, done)
                assert ("is unreadable" in done.stderr) != whole, (oct(mode), words, done.stderr)
            assert (run(home, "ls", rx).stdout.splitlines() == [str(rx / "f")]) == whole
        finally:
            rx.chmod(0o755)


def check_r6_a_link_loop_in_a_walk_is_said(data, workspace):
    """A link that loops is not a link to nothing: `verify`, `reindex` and
    `sweep` name it where they pass over one that dangles in silence."""
    home, w = fresh(workspace, "r6-loop")
    tree = w / "t"
    tree.mkdir()
    (tree / "f").write_text("f")
    (tree / "dangling").symlink_to("nowhere")
    (tree / "notdir").symlink_to("f/x")
    assert run(home, "verify", tree).returncode == 0
    (tree / "l1").symlink_to("l2")
    (tree / "l2").symlink_to("l1")
    checked = run(home, "verify", tree)
    assert checked.returncode == 1, checked
    assert sorted(line.split("\t")[:2] for line in checked.stdout.splitlines()) == \
        [["unreadable", str(tree / "l1")], ["unreadable", str(tree / "l2")]], checked.stdout
    rebuilt = run(home, "reindex", tree)
    assert rebuilt.returncode == 1 and rebuilt.stderr.count("it was not reindexed") == 2, rebuilt
    swept = json.loads(run(home, "--json", "sweep", tree).stdout)
    assert sorted(swept["unreadable"]) == [str(tree / "l1"), str(tree / "l2")], swept


def check_r6_sweep_counts_what_it_left(data, workspace):
    """`skipped` counts the rows under a root it did not sweep because it is
    away; a dry run counts the list it would write; `sweep <dir>` builds the
    list of a root declared inside it."""
    home, w = fresh(workspace, "r6-sweep-counts")
    (w / "disk" / "inner").mkdir(parents=True)
    (w / "disk" / "f").write_text("f")
    (w / "disk" / ".marker").write_text("m")
    assert run(home, "tag", "add", "-t", "t", "--", w / "disk/f").returncode == 0
    assert run(home, "roots", "add", "--id", "disk", "--check", ".marker",
               w / "disk").returncode == 0
    (w / "disk" / ".marker").unlink()
    (w / "disk" / "f").unlink()
    for dry in ((), ("-n",)):
        swept = json.loads(run(home, *dry, "--json", "sweep").stdout)
        assert swept["skipped"] == 1 and swept["gone"] == [], (dry, swept)
    assert run(home, "roots", "rm", "--id", "disk").returncode == 0

    (w / "top" / "inner").mkdir(parents=True)
    (w / "top" / "a").write_text("a")
    (w / "top" / "inner" / "b").write_text("b")
    assert run(home, "roots", "add", "--id", "top", w / "top").returncode == 0
    assert run(home, "roots", "add", "--id", "inner", w / "top/inner").returncode == 0
    planned = json.loads(run(home, "-n", "--json", "sweep", w / "top").stdout)
    done = json.loads(run(home, "--json", "sweep", w / "top").stdout)
    assert planned["listed"] == done["listed"] == 3, (planned, done)
    assert done["trees"] == [str(w / "top"), str(w / "top/inner")], done
    cached = run(home, "ls", "--untagged", "--cached", w / "top/inner")
    assert cached.returncode == 0 and cached.stdout.splitlines() == [str(w / "top/inner/b")], \
        cached


def check_r6_a_view_is_named_as_it_is_saved(data, workspace):
    """`view rm` reads the name the way `view add` does, and a hand-edited
    line names the key that is wrong."""
    home, _ = fresh(workspace, "r6-views")
    assert run(home, "view", "add", "--name", "  Hi  ", "--tag", "a").stdout == "Hi\n"
    gone = run(home, "view", "rm", "--name", "  hi  ")
    assert gone.returncode == 0, gone
    assert run(home, "view", "ls").stdout == ""
    views = home / "config" / "omafiles" / "views.jsonl"
    views.write_text(json.dumps({"name": "v", "dir": "rel", "tags": []}) + "\n")
    said = run(home, "view", "ls")
    assert said.returncode == 3 and "its dir is not an absolute path" in said.stderr, said


def check_r6_recent_says_each_file_once(data, workspace):
    """`recent --desktop` notes a file nobody may look at once, however many
    entries or spellings name it; a file no path here can spell is left out
    whether the entry or a link names it. An undated row nobody may look at is
    still counted as undated."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r6-recent")
    shut = w / "shut"
    shut.mkdir()
    (shut / "x").write_text("x")
    bad = os.fsencode(w) + b"/bad\xff"
    open(bad, "w").close()
    os.symlink(b"bad\xff", os.fsencode(w / "lnk"))
    (home / "recently-used.xbel").write_text(f"""<?xml version="1.0"?>
<xbel version="1.0">
  <bookmark href="file://{shut}/x" visited="2026-09-01T10:00:00Z"/>
  <bookmark href="file://{shut}/x" visited="2026-09-02T10:00:00Z"/>
  <bookmark href="file://{w}/./shut/x" visited="2026-09-03T10:00:00Z"/>
  <bookmark href="file://{w}/lnk" visited="2026-09-04T10:00:00Z"/>
  <bookmark href="file://{w}/bad%FF" visited="2026-09-05T10:00:00Z"/>
</xbel>
""")
    write_index(home, [{"path": str(shut / "x"), "tags": ["a"]}])
    shut.chmod(0o000)
    try:
        opened = run(home, "recent", "--desktop")
        undated = run(home, "recent")
    finally:
        shut.chmod(0o755)
    assert opened.returncode == 1 and opened.stdout == "", opened
    assert opened.stderr.count("Permission denied") == 1, opened.stderr
    assert "lnk" not in opened.stderr and "bad" not in opened.stderr, opened.stderr
    assert "1 row(s) carry no tagged stamp" in undated.stderr, undated.stderr


def check_r6m_the_file_verbs_count_in_the_parse(data, workspace):
    """`mv`, `cp`, `rm`, `trash`, `chmod` and `batch` count their operands in
    the parse, so completion offers no path past the last one they take; a
    rename flag makes `mv` a set of one path or more."""
    home, w = fresh(workspace, "r6m-file-counts")
    for words, said in ((("mv", "a"), "mv: takes exactly two paths, <old> and <new>"),
                        (("cp", "a", "b", "c"), "cp: takes exactly two paths, <old> and <new>"),
                        (("rm",), "rm: path required"),
                        (("trash",), "trash: path required"),
                        (("chmod", "644"), "chmod: a mode and at least one path required"),
                        (("batch", "a", "b"), "batch: one file at most"),
                        (("mv", "--pattern", "{n}"), "mv: path required")):
        refused = run(home, *words, cwd=w)
        assert refused.returncode == 1 and refused.stderr == said + "\n", (words, refused)

    def offered(*words):
        done = run(home, "__complete", "--", *words, cwd=w)
        return done.returncode, done.stdout.split()

    for words in (("mv", "a", "b", ""), ("cp", "a", "b", ""), ("batch", "a", "")):
        assert offered(*words) == (0, []), (words, offered(*words))
    for words in (("mv", "a", ""), ("mv", "--pattern", "{n}", "a", "b", ""), ("rm", "a", "b", ""),
                  ("trash", ""), ("chmod", "644", "a", ""), ("batch", "")):
        assert offered(*words)[0] == 1, (words, offered(*words))


def check_r6m_a_file_verb_spells_what_was_typed(data, workspace):
    """A file verb or a batch line names what was typed on one line, as every
    refusal does: the line a batch refuses, a `--pattern` token, a destination
    two operands share, a mode, and the file an `import` line names."""
    home, w = fresh(workspace, "r6m-file-spelled")
    for name in ("f", "a1", "b1"):
        (w / name).write_text(name)
    (w / "b\r").write_bytes(b"import b\r\r\n")
    for words, stdin, said in (
            (("batch", "-"), "tag add --tag x -- nosuch\r\r\n",
             "  tag add --tag x -- nosuch\\x0d\n"),
            (("batch", "b\r"), None, "import b\\x0d reads the file this batch"),
            (("mv", "--pattern", "{a\rb}", "f"), None, "unknown token {a\\x0db}"),
            (("mv", "--pattern", "x\ry", "a1", "b1"), None, f"would both become {w}/x\\x0dy"),
            (("chmod", "u+s,\ny", "f"), None, "chmod: u+s,\\ny asks for setuid")):
        refused = run(home, *words, cwd=w, stdin=stdin)
        assert refused.returncode in (1, 2), (words, refused)
        assert said in refused.stderr, (words, refused.stderr)
        assert "\r" not in refused.stderr, (words, refused.stderr)
        assert all(line.startswith(("batch: ", "  ", "tag add: ", "mv: ", "chmod: "))
                   for line in refused.stderr.splitlines()), (words, refused.stderr)


def check_r7_a_root_at_the_top_keeps_the_whole_path(data, workspace):
    """A root declared at `/` stores every row with the whole path below it,
    so the row answers `ls`, `verify` and `prune`, and `roots rm` writes the
    same absolute path back."""
    home, w = fresh(workspace, "r7-root-top")
    f = w / "f"
    f.write_text("f")
    index = home / "omafiles" / "index.jsonl"
    assert run(home, "roots", "add", "--id", "s", "/").returncode == 0
    assert run(home, "tag", "add", "-t", "keep", f).returncode == 0
    row = json.loads(index.read_text())
    assert (row["root"], row["path"]) == ("s", str(f)[1:]), row
    listed = run(home, "ls", "--tag", "keep")
    assert listed.returncode == 0 and listed.stdout == f"{f}\n", listed
    checked = run(home, "verify", w)
    assert checked.returncode == 0 and checked.stdout == "", checked
    pruned = run(home, "--dry-run", "prune", "--all")
    assert pruned.returncode == 0 and pruned.stdout == "", pruned
    assert run(home, "roots", "rm", "--id", "s").returncode == 0
    assert json.loads(index.read_text())["path"] == str(f), index.read_text()


def check_r7_a_full_disk_names_the_systems_reason(data, workspace):
    """An index that fits its lock and not its rewrite fails with ENOSPC's
    words, the system's reason, and the verb is undone as its spec says."""
    home, w = fresh(workspace, "r7-full-disk")
    names = [w / f"f{i}" for i in range(120)]
    for name in names:
        name.write_text("")
    assert run(home, "tag", "add", "-t", "aaaa", *names).returncode == 0
    kept = home / "omafiles"
    saved = w.parent / "saved.jsonl"
    shutil.copy(kept / "index.jsonl", saved)
    # Two pages free past the index: the lock fits, a second copy of an index
    # of more than one page does not.
    page = os.sysconf("SC_PAGESIZE")
    pages = -(-saved.stat().st_size // page) + 2
    done = in_mounts(home, f"mount -t tmpfs -o size={pages * page} tmpfs {kept}", f"""
        cp {saved} {kept}/index.jsonl || exit 97
        "$CLI" tag add -t b -- {w}/f2; echo "tag=$?"
        "$CLI" -y rm -- {w}/f1; echo "rm=$?"
        ls -A {kept}""")
    if done is None:
        return
    # Nothing but the index is left in the directory: no temporary file.
    assert done.stdout.startswith("tag=3\n") and done.stdout.endswith("rm=3\nindex.jsonl\n"), done
    assert done.stderr.count(f"store: cannot write {kept}/index.jsonl: No space left on device") == 2, \
        done.stderr
    assert "Unknown error" not in done.stderr, done.stderr
    assert not (w / "f1").exists()


def check_r7_an_index_that_may_not_be_written_is_found_first(data, workspace):
    """A read-only index is refused when the lock is taken, with the
    system's reason, before `rm` removes anything."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r7-read-only-index")
    (w / "a").write_text("a")
    (w / "b").write_text("b")
    assert run(home, "tag", "add", "-t", "t", w / "b").returncode == 0
    index = home / "omafiles" / "index.jsonl"
    index.chmod(0o444)
    try:
        removed = run(home, "-y", "rm", w / "a")
    finally:
        index.chmod(0o644)
    assert removed.returncode == 3 and removed.stdout == "", removed
    assert removed.stderr == f"store: cannot write {index}: Permission denied\n", removed.stderr
    assert (w / "a").exists()


def check_r7_a_pipe_on_stdin_is_not_missing(data, workspace):
    """`/dev/stdin` fed by a pipe resolves to no name, which is not a path
    that is not there: exit 1, said as a pipe."""
    home, _ = fresh(workspace, "r7-stdin-pipe")
    asked = run(home, "has", "-t", "k", "/dev/stdin", stdin="x")
    assert asked.returncode == 1 and asked.stdout == "", asked
    assert asked.stderr == "has: /dev/stdin is a pipe with no name on a filesystem\n", asked.stderr


def check_r7_a_root_now_a_symlink_is_away_where_typed(data, workspace):
    """An operand typed through a declared root whose directory is now a
    symlink is under that root, which is away, whatever the link resolves to."""
    home, w = fresh(workspace, "r7-root-link")
    r = w / "r"
    r.mkdir()
    (r / "f").write_text("f")
    assert run(home, "roots", "add", "--id", "rr", r).returncode == 0
    assert run(home, "tag", "add", "-t", "t", r / "f").returncode == 0
    index = home / "omafiles" / "index.jsonl"
    before = index.read_text()
    r.rename(w / "r.real")
    r.symlink_to("r.real")
    for words in (("--dry-run", "rm", r / "f"), ("tag", "add", "-t", "u", r / "f"),
                  ("has", "-t", "t", r / "f")):
        refused = run(home, *words)
        assert refused.returncode == 1, (words, refused)
        assert refused.stderr.endswith(f"{r}/f: root rr is declared and not here\n"), \
            (words, refused.stderr)
    assert index.read_text() == before


def check_r7_a_value_is_split_before_it_is_decoded(data, workspace):
    """A value holding one word that is not UTF-8 keeps its other words as
    tags: they are adopted, and only that word is named."""
    home, w = fresh(workspace, "r7-value-bytes")
    c = w / "c"
    c.write_text("c")
    if not carries_xattrs(c):
        return
    os.setxattr(c, "user.omafiles.tags", b"foo,\xffa")
    added = run(home, "tag", "add", "-t", "z", c)
    assert added.returncode == 0, added
    assert "(\\xffa)" in added.stderr and "foo" not in added.stderr, added.stderr
    assert run(home, "tag", "ls", c).stdout == "foo\nz\n"


def check_r7_a_fifo_holds_no_value_silently(data, workspace):
    """A fifo can hold no `user.*` value (xattr(7)), so its row is the only
    record and nothing is said about the xattr, by the tag verbs or `verify`."""
    home, w = fresh(workspace, "r7-fifo")
    fifo = w / "p"
    os.mkfifo(fifo)
    for tag in ("z", "y"):
        added = run(home, "tag", "add", "-t", tag, fifo)
        assert added.returncode == 0 and added.stderr == "", added
    checked = run(home, "verify", w)
    assert checked.returncode == 0 and checked.stdout == "", checked
    assert run(home, "tag", "ls", fifo).stdout == "y\nz\n"


def check_r7_the_editor_is_known_by_its_program(data, workspace):
    """A terminal entry goes to `omarchy-launch-editor` where the program its
    `Exec` starts is an editor, past `env`, and not where only its id holds an
    editor's name. A dry run launches nothing."""
    if not shutil.which("omarchy-launch-editor") or not shutil.which("xdg-mime"):
        return
    home, w = fresh(workspace, "r7-editor")
    apps = home / "applications"
    apps.mkdir()
    (apps / "decoder.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=D\nExec=env X=1 /usr/bin/true %F\n"
        "Terminal=true\nMimeType=text/plain;\n")
    (apps / "writer.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=W\nExec=env TERM=xterm /usr/bin/nvim %F\n"
        "Terminal=true\nMimeType=text/plain;\n")
    (home / "config").mkdir()
    notes = w / "n.txt"
    notes.write_text("hi")
    for entry, editor in (("decoder.desktop", False), ("writer.desktop", True)):
        (home / "config" / "mimeapps.list").write_text(
            f"[Default Applications]\ntext/plain={entry}\n")
        planned = run(home, "--dry-run", "--json", "open", notes)
        assert planned.returncode == 0, planned
        plan = json.loads(planned.stdout)[0]
        if plan["desktop"] != entry:
            return
        assert plan["program"].endswith("/omarchy-launch-editor") == editor, (entry, plan)


def kdl_flags():
    """Every leaf command of the usage file, as the help spells it, to the long
    flags the file declares for it; an alias is its command's."""
    flags, path = {}, []
    for line in (ROOT / "omafiles.usage.kdl").read_text().splitlines():
        found = re.match(r"^(\s*)(cmd|flag|alias) \"([^\"]+)\"", line)
        if not found:
            continue
        depth, kind, name = len(found.group(1)) // 2, found.group(2), found.group(3)
        if kind == "cmd":
            path = path[:depth] + [name]
            flags[" ".join(path)] = set()
            if depth:
                flags.pop(" ".join(path[:depth]), None)
        elif kind == "alias":
            flags[" ".join(path[:-1] + [name])] = flags[" ".join(path)]
        elif depth:
            flags[" ".join(path[:depth])] |= set(re.findall(r"--[a-z][a-z-]*", name))
    return flags


def check_r7_help_names_every_flag_the_parse_takes(data, workspace):
    """Each verb's usage lines name every flag the usage file declares for it,
    the parse takes each of them, and completion, which reads the parse's own
    table, offers none beside them: so help, the file and the parse cannot
    drift apart. A verb's help speaks of that verb alone."""
    home, w = fresh(workspace, "r7-help-flags")
    globals_ = {"--dry-run", "--help", "--json", "--quiet", "--version", "--yes"}
    for verb, declared in kdl_flags().items():
        page = run(home, *verb.split(), "--help")
        assert page.returncode == 0, (verb, page)
        usage = [line for line in page.stdout.splitlines()
                 if line.startswith(("Usage: omafiles ", "       omafiles "))]
        assert usage and all(f" {verb}" in line for line in usage), (verb, page.stdout)
        said = set(re.findall(r"--[a-z][a-z-]*", " ".join(usage)))
        assert declared <= said, (verb, declared - said)
        offered = run(home, "__complete", "--", *verb.split(), "-")
        assert set(offered.stdout.split()) - globals_ <= declared, (verb, offered.stdout)
        for flag in declared:
            taken = run(home, "-n", *verb.split(), flag, "value", cwd=w, stdin="")
            assert "unexpected" not in taken.stderr and "unknown flag" not in taken.stderr, \
                (verb, flag, taken.stderr)
    for verb in ("rm", "trash", "mv", "cp"):
        page = run(home, verb, "--help").stdout
        prose = re.sub(r"--[a-z][a-z-]*", "", page)
        assert "both" not in prose and "neither" not in prose, (verb, page)


def check_r7_the_bash_shim_offers_each_word_as_one(data, workspace):
    """A candidate holding a space is inserted quoted, and a file name holding
    a glob character is offered as itself, not as the names it matches."""
    bash = shutil.which("bash")
    if not bash:
        return
    home, w = fresh(workspace, "r7-bash-quoting")
    (w / "[ab].txt").write_text("g")
    (w / "a.txt").write_text("a")
    assert run(home, "view", "add", "--name", "my view", "--tag", "x").returncode == 0
    script = run(home, "completions", "bash").stdout
    probe = script + 'omafiles() { "$OMAFILES" "$@"; }\n' + "".join(
        f"COMP_WORDS=({words}); COMP_CWORD={cword}; _omafiles omafiles; "
        'printf "[%s]\\n" "${COMPREPLY[@]}"; echo .\n'
        for words, cword in (("omafiles view rm --name my", 4),
                             ("omafiles open '['", 2),
                             ("omafiles tag add --tag zz", 4)))
    done = subprocess.run([bash, "-c", probe], env={**cli_env(home), "OMAFILES": str(CLI)},
                          cwd=str(w), text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
    assert done.stdout.splitlines() == ["[my\\ view]", ".", "[[ab].txt]", ".", "[]", "."], done


def check_r7_completion_reads_a_value_before_the_globals(data, workspace):
    """A word a flag takes as its value is that value, as the run reads it, so
    `--json` after `--from` is not a global already said."""
    home, _ = fresh(workspace, "r7-complete-value")
    offered = run(home, "__complete", "--", "tag", "cp", "--from", "--json", "-")
    assert offered.returncode == 0 and "--json" in offered.stdout.split(), offered
    offered = run(home, "__complete", "--", "-j", "tag", "cp", "--from", "x", "-")
    assert "--json" not in offered.stdout.split(), offered


def check_r7_no_operand_reads_a_closed_stdin(data, workspace):
    """With stdin closed, every operand that reaches it -- a path, a `[dir]`, a
    view's or a root's folder -- is refused as `-` is, and nothing is written
    about the /dev/null standing there."""
    home, w = fresh(workspace, "r7-closed-stdin")
    for words in (("tag", "add", "-t", "y", "/dev/stdin"), ("tag", "clear", "/dev/fd/0"),
                  ("-j", "has", "-t", "y", "/proc/self/fd/0"), ("json", "/dev/stdin"),
                  ("chmod", "644", "/dev/stdin"), ("ls", "--untagged", "/dev/stdin"),
                  ("tags", "/dev/stdin"), ("view", "add", "--name", "v", "/dev/stdin"),
                  ("roots", "add", "--id", "r", "/dev/stdin")):
        done = run(home, *words, cwd=w, preexec_fn=lambda: os.close(0))
        assert done.returncode == 1, (words, done)
        assert "cannot read stdin: Bad file descriptor" in done.stderr, (words, done.stderr)
        assert done.stdout == "", (words, done.stdout)
    assert not (home / "omafiles" / "index.jsonl").exists() \
        or "/dev/null" not in (home / "omafiles" / "index.jsonl").read_text()
    assert not (home / "config" / "omafiles").exists() \
        or not list((home / "config" / "omafiles").iterdir())


def check_r7_a_walk_finds_a_root_turned_link_away(data, workspace):
    """A declared root whose directory is now a symlink is away, and a walk
    that meets its path says so and exits 1 rather than list it as a link."""
    home, w = fresh(workspace, "r7-root-link-walk")
    (w / "r").mkdir()
    (w / "r" / "f").write_text("f")
    assert run(home, "roots", "add", "--id", "rr", w / "r").returncode == 0
    (w / "r").rename(w / "r.real")
    (w / "r").symlink_to("r.real")
    for words in (("ls", "--untagged", "."), ("reindex", ".")):
        done = run(home, *words, cwd=w)
        assert done.returncode == 1, (words, done)
        assert "root rr is declared and not here" in done.stderr, (words, done.stderr)


def check_r7_trash_and_chmod_leave_what_omafiles_keeps(data, workspace):
    """`trash` leaves a root's directory, and one holding it, as `mv` does;
    `chmod` leaves a kept file, though not the folder holding one; `cp` asks
    about the file a link leads to."""
    home, w = fresh(workspace, "r7-kept-and-roots")
    (w / "h" / "r").mkdir(parents=True)
    (w / "f").write_text("f")
    assert run(home, "roots", "add", "--id", "rr", w / "h" / "r").returncode == 0
    assert run(home, "tag", "add", "-t", "k", "--", w / "f").returncode == 0
    for target, said in ((w / "h" / "r", "it is root rr"), (w / "h", "it holds root rr")):
        done = run(home, "-n", "trash", target)
        assert done.returncode == 1 and said in done.stderr, (target, done)
        assert "`omafiles roots rm --id rr` first" in done.stderr, done
    index = home / "omafiles" / "index.jsonl"
    mode = index.stat().st_mode
    refused = run(home, "chmod", "444", index)
    assert refused.returncode == 1 and "omafiles keeps it" in refused.stderr, refused
    assert index.stat().st_mode == mode
    assert run(home, "-n", "chmod", "755", index.parent).returncode == 0
    (w / "sl").symlink_to(index)
    copied = run(home, "cp", w / "sl", w / "z")
    assert copied.returncode == 1 and "omafiles keeps it" in copied.stderr, copied
    assert not (w / "z").exists()


def check_r7_a_folded_destination_drops_its_stale_row(data, workspace):
    """On a directory that folds case, a row spelled in another case at a free
    destination names it, so `mv` and `cp` drop it as they drop a stale row
    spelled the same, and say so."""
    home, w = fresh(workspace, "r7-casefold-stale")
    cf = w / "cf"
    cf.mkdir()
    write_index(home, [{"path": f"{cf}/FOO", "tags": ["old"]},
                       {"path": f"{cf}/BAZ", "tags": ["old"]}])
    done = in_mounts(home, f"mount -t tmpfs -o casefold tmpfs {cf} && chattr +F {cf}", f"""
cd {cf}
touch bar
"$CLI" tag add -t new bar >/dev/null
"$CLI" mv bar foo >/dev/null
"$CLI" cp foo baz >/dev/null
"$CLI" export
""")
    if done is None:
        return
    assert done.returncode == 0, done
    rows = {json.loads(line)["path"]: json.loads(line)["tags"] for line in done.stdout.splitlines()}
    assert rows == {f"{cf}/foo": ["new"], f"{cf}/baz": ["new"]}, done
    for stale in ("FOO", "BAZ"):
        assert f"{cf}/{stale} had a row for a file that is gone" in done.stderr, done.stderr


def check_r7_an_empty_batch_still_reads_the_index(data, workspace):
    """A batch with no line to run reads the index as every verb does, so a
    malformed one is exit 3; it is neither locked nor rewritten."""
    home, _ = fresh(workspace, "r7-empty-batch")
    (home / "omafiles").mkdir(parents=True)
    index = home / "omafiles" / "index.jsonl"
    index.write_text("not json\n")
    for body in ("", "# only a comment\n\n"):
        done = run(home, "batch", stdin=body)
        assert done.returncode == 3 and done.stdout == "", (body, done)
    assert index.read_text() == "not json\n"
    assert not (home / "omafiles" / "index.jsonl.lock").exists()


def check_r7_a_shut_dir_takes_a_batch_back(data, workspace):
    """`reindex` of a `[dir]` it cannot open is a refused operand, so a batch
    holding it writes nothing, as one holding `prune` of it does."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r7-shut-reindex")
    (w / "rb.txt").write_text("r")
    (w / "shut").mkdir()
    (w / "shut").chmod(0o000)
    try:
        done = run(home, "batch", cwd=w, stdin="tag add --tag s1 rb.txt\nreindex shut\n")
        alone = run(home, "reindex", "shut", cwd=w)
    finally:
        (w / "shut").chmod(0o755)
    assert done.returncode == 1 and "failed, nothing was written" in done.stderr, done
    assert run(home, "json", "--", w / "rb.txt").stdout == ""
    assert alone.returncode == 1 and "is unreadable; nothing under it was reindexed" \
        in alone.stderr, alone


def check_r7_reindex_names_only_rows_it_wrote(data, workspace):
    """A value holding a word that is not a tag is left as it is, so a row whose
    tags stay is not in the receipt, run after run, in either form."""
    home, w = fresh(workspace, "r7-reindex-receipt")
    if not carries_xattrs(w):
        return
    (w / "t").mkdir()
    f = w / "t" / "f"
    f.write_text("f")
    assert run(home, "tag", "add", "-t", "keep", "--", f).returncode == 0
    os.setxattr(f, "user.omafiles.tags", b"keep,Bad W")
    for words in (("reindex",), ("reindex", "--from-xattr"), ("-j", "reindex")):
        done = run(home, *words, w / "t")
        assert done.returncode == 1 and "left as it is" in done.stderr, (words, done)
        assert done.stdout in ("", "[]\n"), (words, done.stdout)
    assert os.getxattr(f, "user.omafiles.tags") == b"keep,Bad W"


def check_r7_a_refusal_spells_every_path(data, workspace):
    """Every path a refusal names is spelled on one line, whatever it holds,
    and a value an environment variable holds is quoted where a space ends it;
    the `tag clear` a note names says it drops the row too."""
    home, w = fresh(workspace, "r7-spelled-paths")
    odd = w / "d\nir"
    odd.mkdir()
    (w / "f").write_text("f")
    (w / "t\rk").write_text("t")
    for words, said in ((("rm", odd), f"rm: {w}/d\\nir is a directory"),
                        (("cp", odd, w / "z"), f"cp: {w}/d\\nir is a directory"),
                        (("mv", w / "f", w / "t\rk"), f"mv: {w}/t\\x0dk already exists"),
                        (("view", "add", "--name", "v", w / "t\rk"),
                         f"view add: {w}/t\\x0dk is not a folder"),
                        (("roots", "add", "--id", "r", w / "t\rk"),
                         f"roots add: {w}/t\\x0dk is not a directory")):
        done = run(home, *words, cwd=w)
        assert done.returncode == 1 and said in done.stderr, (words, done.stderr)
        assert "\r" not in done.stderr and done.stderr.count("\n") == 1, (words, done.stderr)
    if os.geteuid() != 0:
        (w / "tr" / "s\rt").mkdir(parents=True)
        (w / "tr" / "s\rt").chmod(0o000)
        try:
            walked = run(home, "reindex", w / "tr")
        finally:
            (w / "tr" / "s\rt").chmod(0o755)
        assert walked.stderr == f"reindex: {w}/tr/s\\x0dt is unreadable; nothing under it was " \
            "reindexed: Permission denied\n", walked.stderr
    env = run(home, "tag", "add", "-t", "x", "--", w / "f",
              extra_env={"OMAFILES_DRY_RUN": "1 "})
    assert env.returncode == 1 and "OMAFILES_DRY_RUN='1 ' is neither" in env.stderr, env
    if not carries_xattrs(w):
        return
    named = w / "n\nm"
    named.write_text("n")
    os.setxattr(named, "user.omafiles.tags", b"other,Bad W")
    done = run(home, "tag", "add", "-t", "keep", "--", named)
    assert done.returncode == 0 and done.stderr.count("\n") == 1, done
    assert f"tag add: {w}/n\\nm carries words" in done.stderr, done.stderr
    assert "drops the value and the row" in done.stderr, done.stderr


def check_r7_a_destination_may_be_the_parent(data, workspace):
    """`..` refused as a source is an ordinary destination directory, as it is
    to coreutils: `mv c ..` and `cp c ..` land in the parent."""
    home, w = fresh(workspace, "r7-parent-destination")
    (w / "sub").mkdir()
    for name in ("c", "d", "e"):
        (w / "sub" / name).write_text(name)
    for words, landed in ((("mv", "c", ".."), w / "c"), (("cp", "d", ".."), w / "d"),
                          (("mv", "e", "../."), w / "e")):
        done = run(home, *words, cwd=w / "sub")
        assert done.returncode == 0 and done.stdout == f"{landed}\n", (words, done)
        assert landed.exists()
    assert not (w / "sub" / "c").exists() and (w / "sub" / "d").exists()



def check_r7_a_message_from_the_core_spells_its_paths(data, workspace):
    """A path the core names -- a file omafiles keeps, a path one holds, a link
    that leads nowhere, a root's -- is spelled on one line; a row path is offered
    in its clean form only where it has no `..`, which cleaning would change."""
    home, w = fresh(workspace, "r7-core\rspelled")
    h = str(home).replace("\r", "\\x0d")
    s = str(w).replace("\r", "\\x0d")
    for row, said in (({"path": f"{w}/a//b", "tags": ["x"]},
                       f"index row path {s}/a//b is not in its clean form, {s}/a/b\n"),
                      ({"path": f"{w}/L/../x", "tags": ["x"]},
                       f"index row path {s}/L/../x is not in its clean form\n")):
        write_index(home, [row])
        done = run(home, "tags")
        assert done.returncode == 3, done
        assert done.stderr == f"store: malformed jsonl at {h}/omafiles/index.jsonl:1: {said}", \
            done.stderr
    (home / "omafiles" / "index.jsonl").unlink()
    (w / "gone").symlink_to("nowhere")
    dangling = run(home, "tag", "ls", "--", w / "gone")
    assert dangling.returncode == 2, dangling
    assert f"{s}/gone is a symlink to {s}/nowhere, which is not there" in dangling.stderr, \
        dangling.stderr
    (w / "r").mkdir()
    (w / "r" / "m").write_text("m")
    (w / "r" / "f").write_text("f")
    assert run(home, "roots", "add", "--id", "r", "--check", "m", "--", w / "r").returncode == 0
    names = home / "cache" / "omafiles" / "names" / "r"
    names.parent.mkdir(parents=True)
    names.write_text("../x\n")
    cached = run(home, "ls", "--untagged", "--cached")
    assert f"names: {h}/cache/omafiles/names/r:1 is not a clean path inside the root" \
        in cached.stderr, cached.stderr
    (w / "r" / "m").unlink()
    away = run(home, "tag", "ls", "--", w / "r" / "f")
    assert away.returncode == 1, away
    assert f"{s}/r/f: root r is declared and not here" in away.stderr, away.stderr
    assert "\r" not in dangling.stderr + cached.stderr + away.stderr, \
        (dangling.stderr, cached.stderr, away.stderr)
    if os.geteuid() == 0:
        return
    (w / "s" / "c").mkdir(parents=True)
    (w / "s" / "c" / "m").write_text("m")
    (w / "s" / "f").write_text("f")
    assert run(home, "roots", "add", "--id", "s", "--check", "c/m", "--", w / "s").returncode == 0
    (w / "s" / "c").chmod(0o000)
    try:
        shut = run(home, "tag", "ls", "--", w / "s" / "f")
    finally:
        (w / "s" / "c").chmod(0o755)
    assert shut.returncode == 1, shut
    assert f"{s}/s/f: root s cannot be checked: " in shut.stderr, shut.stderr
    assert "\r" not in shut.stderr, shut.stderr


def check_r8_a_rooted_name_may_begin_with_a_byte_order_mark(data, workspace):
    """A row stored under a root holds a U+FEFF that begins its relative path,
    and so does a root's `--check`: each reads back as the name it is, from the
    index, from a dump in either shape and from the roots."""
    home, w = fresh(workspace, "r8-rooted-bom")
    if not carries_xattrs(w):
        return
    marked = w / "﻿bom"
    marked.write_text("m")
    (w / "bom").write_text("b")
    os.setxattr(marked, "user.omafiles.tags", b"secret")
    (w / "﻿m").write_text("")
    assert run(home, "roots", "add", "--id", "w", "--check", "﻿m", "--", w).returncode == 0
    assert run(home, "roots", "ls").stdout == f"w\t{w}\there\n"
    assert run(home, "reindex", w).returncode == 0
    assert run(home, "ls", "--tag", "secret").stdout == f"{marked}\n"
    verified = run(home, "verify", w)
    assert verified.returncode == 0 and verified.stdout == "", verified
    assert run(home, "reindex", w).returncode == 0
    assert "user.omafiles.tags" not in os.listxattr(w / "bom")
    assert run(home, "tag", "ls", "--", marked).stdout == "secret\n"
    for shape in ((), ("--json",)):
        dump = run(home, *shape, "export").stdout
        (home / "omafiles" / "index.jsonl").unlink()
        imported = run(home, "import", "-", stdin=dump)
        assert imported.returncode == 0, (shape, imported)
        assert run(home, "ls", "--tag", "secret").stdout == f"{marked}\n", shape


def check_r8_mv_and_cp_carry_the_values_tags_into_the_row(data, workspace):
    """The row that lands at the far end of `mv` or `cp` is the source's row
    united with the valid tags its value carries, so a filesystem with no
    `user.*` namespace, where that row is the only copy, loses none."""
    home, w = fresh(workspace, "r8-landing-union")
    if not carries_xattrs(w):
        return
    rf = w / "rf"
    rf.mkdir()
    for name in ("a", "c"):
        (w / name).write_text(name)
        assert run(home, "tag", "add", "-t", "old", "--", w / name).returncode == 0
        os.setxattr(w / name, "user.omafiles.tags", b"old,extra")
    done = in_mounts(home, f"mount -t ramfs ramfs {rf}", f"""
"$CLI" --json mv {w}/a {rf}/a; echo "mv $?"
"$CLI" --json cp {w}/c {rf}/c; echo "cp $?"
"$CLI" ls --tag extra; echo "ls $?"
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    assert [row["tags"] for row in json.loads(lines[0])["rows"]] == [["extra", "old"]], done
    assert [row["tags"] for row in json.loads(lines[2])["rows"]] == [["extra", "old"]], done
    assert lines[1] == "mv 0" and lines[3] == "cp 0", done
    assert lines[4:] == [f"{rf}/a", f"{rf}/c", "ls 0"], done


def check_r8_a_dry_run_says_a_stale_row_would_be_dropped(data, workspace):
    """The note about a row whose file is gone is true of a dry run, which
    drops nothing, and of the real run."""
    home, w = fresh(workspace, "r8-stale-tense")
    (w / "g").write_text("g")
    (w / "a").write_text("a")
    assert run(home, "tag", "add", "-t", "old", "--", w / "g").returncode == 0
    (w / "g").unlink()
    index = home / "omafiles" / "index.jsonl"
    before = index.read_text()
    for verb in ("cp", "mv"):
        dry = run(home, "-n", verb, w / "a", w / "g")
        assert dry.returncode == 0, dry
        assert dry.stderr == f"{verb}: {w}/g had a row for a file that is gone; landing there " \
            "would drop it and its tags old\n", dry.stderr
        assert index.read_text() == before
    real = run(home, "mv", w / "a", w / "g")
    assert real.stderr == f"mv: {w}/g had a row for a file that is gone; landing there " \
        "drops it and its tags old\n", real.stderr
    assert "old" not in index.read_text()
    # A rename of a set that does not land drops nothing where it would have.
    if os.geteuid() == 0:
        return
    (w / "d").mkdir()
    (w / "d" / "a").write_text("a")
    (w / "d" / "b").write_text("b")
    assert run(home, "tag", "add", "-t", "old", "--", w / "d" / "b").returncode == 0
    (w / "d" / "b").unlink()
    (w / "d").chmod(0o555)
    try:
        failed = run(home, "mv", "--pattern", "b", "--", w / "d" / "a")
    finally:
        (w / "d").chmod(0o755)
    assert failed.returncode == 1 and "had a row" not in failed.stderr, failed
    assert f"{w}/d/b" in index.read_text()


def check_r8_recent_desktop_orders_within_a_second(data, workspace):
    """GLib stamps a bookmark in microseconds, and two files opened within one
    second are newest first by them, whichever the list names first."""
    home, w = fresh(workspace, "r8-subsecond")
    for name in ("a", "b"):
        (w / name).write_text(name)
    for first, second in (("a", "b"), ("b", "a")):
        (home / "recently-used.xbel").write_text(f"""<?xml version="1.0"?>
<xbel version="1.0">
  <bookmark href="file://{w}/{first}" visited="2026-09-03T10:00:00.100000Z"/>
  <bookmark href="file://{w}/{second}" visited="2026-09-03T10:00:00.900000Z"/>
</xbel>
""")
        done = run(home, "recent", "--desktop", "1")
        assert done.returncode == 0 and done.stdout == f"{w}/{second}\n", (first, done)


def check_r8_an_href_with_a_query_names_no_file(data, workspace):
    """A `?` in a `file:` href begins a query, which GLib and Qt drop, so the
    entry would name another file: it is skipped, as one holding `#` is."""
    home, w = fresh(workspace, "r8-href-query")
    for name in ("a", "a?b", "a#b"):
        (w / name).write_text(name)
    (home / "recently-used.xbel").write_text(f"""<?xml version="1.0"?>
<xbel version="1.0">
  <bookmark href="file://{w}/a?b" visited="2026-09-03T11:00:00Z"/>
  <bookmark href="file://{w}/a#b" visited="2026-09-03T11:00:00Z"/>
  <bookmark href="file://{w}/a%3Fb" visited="2026-09-03T10:00:00Z"/>
</xbel>
""")
    done = run(home, "recent", "--desktop")
    assert done.returncode == 0 and done.stdout == f"{w}/a?b\n", done


def check_r8_a_trailing_newline_is_not_part_of_a_word(data, workspace):
    """A count, a duration and a mode followed by a newline are refused, and a
    name ending in one is quoted in the line a dry run prints to paste."""
    home, w = fresh(workspace, "r8-anchored")
    (w / "f").write_text("f")
    for words in (("recent", "1\n"), ("ls", "--since", "1d\n", w),
                  ("chmod", "644\n", "--", w / "f")):
        refused = run(home, *words)
        assert refused.returncode == 1 and refused.stdout == "", (words, refused)
    (w / "x\n").write_text("x")
    dry = run(home, "--dry-run", "open", w / "x\n")
    assert dry.returncode == 0 and dry.stdout.count("\n") == 1, dry
    unspelled = run_bytes(home, "ls", "-t", b"\xff")
    assert b"no path, tag or name here can be spelled from it" in unspelled.stderr, unspelled


def check_r8_a_mount_point_is_not_moved(data, workspace):
    """rename(2) answers EBUSY for a mount point, so `mv` refuses one, alone or
    in a set, in a dry run as for real."""
    home, w = fresh(workspace, "r8-mount-point")
    (w / "src" / "d").mkdir(parents=True)
    (w / "src" / "f").write_text("f")
    (w / "m").mkdir()
    (w / "ff").write_text("")
    done = in_mounts(home, f"mount --bind {w}/src/d {w}/m && mount --bind {w}/src/f {w}/ff", f"""
for dry in -n ""; do
  "$CLI" $dry mv {w}/m {w}/m2 2>&1; echo "dir $?"
  "$CLI" $dry mv {w}/ff {w}/src/g 2>&1; echo "file $?"
  "$CLI" $dry mv --pattern '{{name}}.x' -- {w}/ff 2>&1; echo "set $?"
done
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    busy = os.strerror(16)
    for dry in (0, 6):
        assert lines[dry:dry + 6] == [
            f"mv: cannot move {w}/m to {w}/m2: {busy}; it is a mount point", "dir 1",
            f"mv: cannot move {w}/ff to {w}/src/g: {busy}; it is a mount point", "file 1",
            f"mv: cannot move {w}/ff to {w}/ff.x: {busy}; it is a mount point", "set 1"], done


def check_r8_a_walk_gap_does_not_fail_a_later_line(data, workspace):
    """A `reindex` line that walked away from a path is the batch's exit 1 after
    its commit, not a later `tag rm` or `tag clear` line failing the run."""
    for verb in (("tag", "rm", "--tag", "a"), ("tag", "clear")):
        home, w = fresh(workspace, f"r8-gap-{verb[1]}")
        if not carries_xattrs(w):
            return
        tree = w / "tree"
        tree.mkdir()
        for name in ("f", "g"):
            (tree / name).write_text(name)
        os.mkdir(os.fsencode(tree) + b"/caf\xe9")
        os.setxattr(tree / "g", "user.omafiles.tags", b"adopt")
        assert run(home, "tag", "add", "--tag", "a", "--", tree / "f").returncode == 0
        job = w / "job"
        job.write_text(f"reindex {tree}\n{' '.join(verb)} -- {tree / 'f'}\n")
        done = run(home, "--json", "batch", job)
        assert done.returncode == 1, done
        assert "failed, nothing was written" not in done.stderr, done.stderr
        assert "cannot be named" in done.stderr, done.stderr
        rows = {row["path"]: row["tags"] for row in json.loads(done.stdout)}
        assert rows == {str(tree / "g"): ["adopt"], str(tree / "f"): []}, rows
        assert run(home, "tag", "ls", "--", tree / "f").stdout == ""


def check_r8_a_value_in_another_spelling_is_reported(data, workspace):
    """A value out of order or holding a tag twice is a spelling this program
    never writes: `verify` reports it word for word and `reindex` rewrites it."""
    home, w = fresh(workspace, "r8-spelling")
    if not carries_xattrs(w):
        return
    x, y, z = w / "x", w / "y", w / "z"
    for path in (x, y, z):
        path.write_text(path.name)
    assert run(home, "tag", "add", "--tag", "t", "--tag", "u", "--", x, y).returncode == 0
    os.setxattr(x, "user.omafiles.tags", b"u,t")
    os.setxattr(y, "user.omafiles.tags", b"t,u,u")
    os.setxattr(z, "user.omafiles.tags", b"u,t")
    seen = run(home, "verify", w)
    assert seen.returncode == 1, seen
    assert seen.stdout.splitlines() == [f"xattr-differs\t{x}\tindex=t,u xattr=u,t",
                                        f"xattr-differs\t{y}\tindex=t,u xattr=t,u,u",
                                        f"unindexed\t{z}\txattr=u,t"], seen.stdout
    assert run(home, "reindex", w).returncode == 0
    for path in (x, y, z):
        assert os.getxattr(path, "user.omafiles.tags") == b"t,u", path
    assert run(home, "verify", w).returncode == 0
    os.setxattr(x, "user.omafiles.tags", b"u,t")
    assert run(home, "reindex", "--from-xattr", w).stdout == f"{x}\n"
    assert os.getxattr(x, "user.omafiles.tags") == b"t,u"
    if os.geteuid() == 0:
        return
    # A copy the filesystem refused changed nothing, so it is no line either.
    os.setxattr(y, "user.omafiles.tags", b"u,t")
    y.chmod(0o444)
    try:
        refused = run(home, "reindex", w)
    finally:
        y.chmod(0o644)
    assert refused.returncode == 1 and refused.stdout == "", refused
    assert "Permission denied" in refused.stderr, refused.stderr


def check_r8_sweep_counts_a_path_once(data, workspace):
    """A row the walk could not describe is `unreadable`, and not `skipped` too."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r8-once")
    shut = w / "d"
    shut.mkdir()
    for name in ("a", "b"):
        (shut / name).write_text(name)
    assert run(home, "tag", "add", "--tag", "k", "--", shut / "a", shut / "b").returncode == 0
    shut.chmod(0o400)
    try:
        swept = run(home, "--json", "sweep", w)
    finally:
        shut.chmod(0o755)
    assert swept.returncode == 0, swept
    found = json.loads(swept.stdout)
    assert found["unreadable"] == [str(shut / "a"), str(shut / "b")], found
    assert found["skipped"] == 0, found


def check_r8_the_sweep_receipt_says_what_is_true(data, workspace):
    """A receipt it could not write claims nothing about the one before it, and
    a count is read back whole."""
    home, w = fresh(workspace, "r8-receipt")
    receipt = home / "state" / "omafiles" / "sweep.json"
    receipt.mkdir(parents=True)
    swept = run(home, "sweep", w)
    assert swept.returncode == 0, swept
    assert f"sweep: cannot write the receipt {receipt}: Is a directory\n" in swept.stderr, \
        swept.stderr
    receipt.rmdir()
    assert run(home, "sweep", w).returncode == 0
    kept = json.loads(receipt.read_text())
    kept["adopted"] = 2**53 + 1
    receipt.write_text(json.dumps(kept) + "\n")
    said = run(home, "sweep", "--status")
    assert f"adopted\t{2**53 + 1}\n" in said.stdout, said.stdout
    assert json.loads(run(home, "--json", "sweep", "--status").stdout)["adopted"] == 2**53 + 1


def check_r8_a_lone_dash_before_the_verb_is_no_flag(data, workspace):
    """`-` is an operand, never a flag, so before the verb it is no command."""
    dashed = run(data, "-", "ls")
    assert dashed.returncode == 1, dashed
    assert dashed.stderr == "omafiles: unknown command: - (try omafiles --help)\n", dashed.stderr


def check_r8_removing_a_nested_root_drops_the_outer_list(data, workspace):
    """The list of the root around a removed one left that root's paths out, so
    it goes too, and `--cached` asks for the sweep that rebuilds it."""
    home, w = fresh(workspace, "r8-nested")
    inner = w / "inner"
    inner.mkdir()
    (inner / "f").write_text("f")
    (w / "g").write_text("g")
    assert run(home, "roots", "add", "--id", "outer", "--", w).returncode == 0
    assert run(home, "roots", "add", "--id", "inner", "--", inner).returncode == 0
    assert run(home, "sweep").returncode == 0
    assert run(home, "roots", "rm", "--id", "inner").returncode == 0
    cached = run(home, "ls", "--untagged", "--cached")
    assert cached.returncode == 1, cached
    assert "root outer has no list of names yet" in cached.stderr, cached.stderr
    assert run(home, "sweep").returncode == 0
    listed = run(home, "ls", "--untagged", "--cached").stdout.splitlines()
    assert str(inner / "f") in listed, listed


def check_r8_completion_has_no_opinion_on_a_refused_line(data, workspace):
    """A flag said twice where it is said once, or beside one it may not be, is
    a line the parse refuses, and completion offers nothing there."""
    home, w = fresh(workspace, "r8-complete")
    (w / "f").write_text("f")
    assert run(home, "tag", "add", "--tag", "w", "--", w / "f").returncode == 0
    assert run(home, "__complete", "--", "has", "--tag", "").stdout == "w\n"
    for words in (("has", "--tag", "w", "--tag", ""),
                  ("ls", "--untagged", "--tag", ""),
                  ("ls", "--since", "1d", "--untagged", "")):
        offered = run(home, "__complete", "--", *words)
        assert offered.returncode == 1 and offered.stdout == "", (words, offered)


def check_r8_the_editor_is_found_past_env_options(data, workspace):
    """`env -u NAME` and `env -i` before the program are env's own, so the
    program after them is what the entry runs. A dry run launches nothing."""
    if not shutil.which("omarchy-launch-editor") or not shutil.which("xdg-mime"):
        return
    home, w = fresh(workspace, "r8-editor")
    apps = home / "applications"
    apps.mkdir()
    (home / "config").mkdir()
    notes = w / "n.txt"
    notes.write_text("hi")
    for exec_line in ("env -u FOO nvim %F", "env -i -C /tmp X=1 nvim %F"):
        (apps / "writer.desktop").write_text(
            f"[Desktop Entry]\nType=Application\nName=W\nExec={exec_line}\n"
            "Terminal=true\nMimeType=text/plain;\n")
        (home / "config" / "mimeapps.list").write_text(
            "[Default Applications]\ntext/plain=writer.desktop\n")
        planned = run(home, "--dry-run", "--json", "open", notes)
        assert planned.returncode == 0, planned
        plan = json.loads(planned.stdout)[0]
        if plan["desktop"] != "writer.desktop":
            return
        assert plan["program"].endswith("/omarchy-launch-editor"), (exec_line, plan)


def check_r8_no_opener_names_both(data, workspace):
    """With neither `xdg-open` nor `gio`, the refusal names both."""
    prefix = hidden_openers()
    if prefix is None:
        return
    home, w = fresh(workspace, "r8-opener")
    (w / "f.txt").write_text("f")
    done = run(home, "--dry-run", "open", w / "f.txt", wrap=prefix)
    if "no xdg-open" not in done.stderr:
        return
    assert done.stderr == "open: no xdg-open or gio to open it with\n", done.stderr


def check_r8_a_write_that_changes_nothing_writes_nothing(data, workspace):
    """A write that changes no row takes the lock and leaves the index as it
    was, and its receipt names only what changed -- a row, or a file's copy."""
    home, w = fresh(workspace, "r8-noop")
    index = home / "omafiles" / "index.jsonl"
    f, g, h = w / "f", w / "g", w / "h"
    for path in (f, g, h):
        path.write_text(path.name)
    # Nothing to write makes no index.
    none = run(home, "--json", "tag", "rm", "--tag", "nope", "--", f)
    assert none.returncode == 0 and json.loads(none.stdout) == [], none
    assert not index.exists()
    assert run(home, "tag", "add", "--tag", "x", "--", f, h).returncode == 0
    written = index.stat()
    for words in (("--json", "tag", "add", "--tag", "x", "--", f),
                  ("--json", "tag", "rm", "--tag", "nope", "--", f),
                  ("--json", "tag", "cp", "--from", f, "--", h),
                  ("--json", "tag", "merge", "nope", "other")):
        done = run(home, *words)
        assert done.returncode == 0 and json.loads(done.stdout) == [], (words, done)
    for words in (("--dry-run", "tag", "add", "--tag", "x", "--", f),
                  ("--dry-run", "tag", "rm", "--tag", "nope", "--", f)):
        done = run(home, *words)
        assert done.returncode == 0 and done.stdout == "", (words, done)
    job = w / "job"
    job.write_text(f"tag rm --tag x -- {f}\ntag add --tag x -- {f}\n")
    batched = run(home, "--json", "batch", job)
    assert batched.returncode == 0 and json.loads(batched.stdout) == [], batched
    dump = w / "dump.json"
    dump.write_text(run(home, "--json", "export").stdout)
    imported = run(home, "--json", "import", dump)
    assert imported.returncode == 0 and json.loads(imported.stdout) == [], imported
    # Nor does a move or a copy of a file with no row.
    (w / "u").write_text("u")
    for words in (("mv", "--", w / "u", w / "u2"), ("cp", "--", w / "u2", w / "u3")):
        done = run(home, *words)
        assert done.returncode == 0, (words, done)
    after = index.stat()
    assert (after.st_ino, after.st_mtime_ns) == (written.st_ino, written.st_mtime_ns), \
        "a write that changed nothing rewrote the index"
    # One that changes a row writes it, and names only that row.
    both = run(home, "--json", "tag", "add", "--tag", "x", "--", f, g)
    assert [row["path"] for row in json.loads(both.stdout)] == [str(g)], both.stdout
    assert index.stat().st_ino != written.st_ino
    if carries_xattrs(w):
        # A file whose copy was lost is a change, and its receipt says so.
        os.removexattr(f, "user.omafiles.tags")
        mended = run(home, "--json", "tag", "add", "--tag", "x", "--", f)
        assert [row["path"] for row in json.loads(mended.stdout)] == [str(f)], mended.stdout
        assert os.getxattr(f, "user.omafiles.tags") == b"x"



def check_r8_a_new_root_waits_for_a_write_that_changes_a_row(data, workspace):
    """Rows under a root declared since they were written keep their absolute
    lines through a write that changes no row, and all move under the root with
    the first write that changes one."""
    home, w = fresh(workspace, "r8-migrate-later")
    index = home / "omafiles" / "index.jsonl"
    f, g = w / "f", w / "g"
    for path in (f, g):
        path.write_text(path.name)
    assert run(home, "tag", "add", "--tag", "x", "--", f, g).returncode == 0
    assert run(home, "roots", "add", "--id", "w", "--", w).returncode == 0
    lines = index.read_text()
    assert run(home, "tag", "add", "--tag", "x", "--", f).returncode == 0
    assert index.read_text() == lines, index.read_text()
    assert run(home, "tag", "add", "--tag", "y", "--", f).returncode == 0
    assert [json.loads(line).get("root") for line in index.read_text().splitlines()] \
        == ["w", "w"], index.read_text()


CASEFOLD = "mount -t tmpfs -o casefold tmpfs {0} && chattr +F {0}"


def check_r9_a_recased_row_follows_the_file(data, workspace):
    """A row spelled in the old case of a folder another program re-cased on a
    directory that folds case is that file's row: every read finds it by either
    spelling, `prune` keeps it, the next write stores the new spelling even
    where no tag changed, and two rows meeting there are one."""
    home, w = fresh(workspace, "r9-recased-row")
    cf = w / "cf"
    cf.mkdir()
    index = home / "omafiles" / "index.jsonl"
    done = in_mounts(home, CASEFOLD.format(cf), f"""
cd {cf}
mkdir Photos; touch Photos/p1.jpg Photos/p2.jpg
"$CLI" tag add -t trip Photos/p1.jpg Photos/p2.jpg >/dev/null
mv Photos x; mv x photos
"$CLI" ls -t trip
"$CLI" tags
"$CLI" has -t trip Photos/p1.jpg && echo has-old
"$CLI" has -t trip PHOTOS/P1.JPG && echo has-other
"$CLI" tag ls Photos/p2.jpg
"$CLI" verify . && echo verified
"$CLI" -n prune . && echo pruned
cat {index}
"$CLI" tag add -t trip photos/p1.jpg >/dev/null
cat {index}
printf '{{"path":"{cf}/Photos/p2.jpg","tags":["old"]}}\\n' >> {index}
"$CLI" tag ls photos/p2.jpg
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    assert lines[:8] == [f"{cf}/photos/p1.jpg", f"{cf}/photos/p2.jpg", "trip\t2", "has-old",
                         "has-other", "trip", "verified", "pruned"], done
    assert [json.loads(line)["path"] for line in lines[8:10]] \
        == [f"{cf}/Photos/p1.jpg", f"{cf}/Photos/p2.jpg"], done
    assert [json.loads(line)["path"] for line in lines[10:12]] \
        == [f"{cf}/photos/p1.jpg", f"{cf}/photos/p2.jpg"], done
    assert lines[12:] == ["old", "trip"], done


def check_r9_a_recased_root_follows_its_directory(data, workspace):
    """A declared root whose folder another program re-cased holds the rows
    under it by either spelling, and `sweep` stores its new spelling."""
    home, w = fresh(workspace, "r9-recased-root")
    cf = w / "cf"
    cf.mkdir()
    declared = home / "config" / "omafiles" / "roots.jsonl"
    done = in_mounts(home, CASEFOLD.format(cf), f"""
cd {cf}
mkdir Music; echo s > Music/Song.mp3
"$CLI" roots add --id m Music >/dev/null
"$CLI" tag add -t t Music/Song.mp3 >/dev/null
mv Music x; mv x music
"$CLI" roots
"$CLI" tag add -t u music/song.mp3 >/dev/null
"$CLI" export
"$CLI" -n sweep >/dev/null 2>&1; cat {declared}
"$CLI" sweep >/dev/null 2>&1; cat {declared}
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    assert lines[0] == f"m\t{cf}/music\there", done
    assert json.loads(lines[1]) | {"tagged": 0} \
        == {"path": "Song.mp3", "root": "m", "tags": ["t", "u"], "tagged": 0}, done
    assert [json.loads(line)["path"] for line in lines[2:]] \
        == [f"{cf}/Music", f"{cf}/music"], done



def check_r9_a_directory_is_told_folding_by_a_name_it_holds(data, workspace):
    """Whether a directory folds case is told once, by the first row's name it
    holds: a name with no letter or a file gone tells nothing, and the row
    after it still follows its re-cased file. A line in a re-cased spelling
    given twice is two answers, as the same spelling twice is, and one given
    once beside the row it follows is that row."""
    home, w = fresh(workspace, "r9-folding-told")
    cf = w / "cf"
    cf.mkdir()
    index = home / "omafiles" / "index.jsonl"
    old, new = f"{cf}/E/c.jpg", f"{cf}/E/C.JPG"
    done = in_mounts(home, CASEFOLD.format(cf), f"""
cd {cf}
mkdir D E; touch D/0 D/b.jpg E/A E/c.jpg
"$CLI" tag add -t t D/0 D/b.jpg E/A E/c.jpg >/dev/null
rm E/A; mv D/b.jpg D/x; mv D/x D/B.JPG; mv E/c.jpg E/x; mv E/x E/C.JPG
"$CLI" has -t t D/B.JPG && echo followed-past-a-caseless-name
"$CLI" has -t t E/C.JPG && echo followed-past-a-gone-file
printf '{{"path":"{old}","tags":["t"]}}\n{{"path":"{old}","tags":["u"]}}\n' > {index}
"$CLI" tags 2>&1; echo "exit $?"
printf '{{"path":"{old}","tags":["t"]}}\n{{"path":"{new}","tags":["u"]}}\n' > {index}
"$CLI" tag ls E/c.jpg
printf '{{"path":"{new}","tags":["u"]}}\n' >> {index}
"$CLI" tags 2>&1; echo "exit $?"
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    assert lines[:2] == ["followed-past-a-caseless-name", "followed-past-a-gone-file"], done
    assert lines[2].endswith(f"index.jsonl:2: {old} is on an earlier line too"), done
    assert lines[3:6] == ["exit 3", "t", "u"], done
    assert lines[6].endswith(f"index.jsonl:3: {new} is on an earlier line too"), done
    assert lines[7:] == ["exit 3"], done


def check_r9_hard_links_one_case_apart_do_not_fold_a_directory(data, workspace):
    """Two names one case apart in one directory are two entries, so it does
    not fold case even where they are links to one file: `mv` onto a free name
    leaves the row of another file spelled in another case where it is."""
    home, w = fresh(workspace, "r9-links-apart")
    d = w / "d"
    d.mkdir()
    # Made last: tmpfs reads a directory newest first, so the links come before `x`.
    for name in ("G", "x", "f"):
        (d / name).write_text(name)
    os.link(d / "f", d / "F")
    assert run(home, "tag", "add", "-t", "g", "--", d / "G").returncode == 0
    assert run(home, "mv", "--", d / "x", d / "g").returncode == 0
    assert run(home, "tag", "ls", "--", d / "G").stdout == "g\n"

def check_r9_a_move_refuses_a_value_it_may_not_read(data, workspace):
    """`mv` and `cp` of a file whose value the system will not show refuse, in
    a dry run too, and one rename of a set refuses alone; nothing moves."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r9-unread-value")
    if not carries_xattrs(w):
        return
    b, other = w / "b", w / "other"
    b.write_text("q")
    other.write_text("o")
    assert run(home, "tag", "add", "-t", "t", "--", b).returncode == 0
    os.setxattr(b, "user.omafiles.tags", b"t,extra")
    b.chmod(0o000)
    try:
        said = f"the tags it carries cannot be read: xattr get failed on {b}: Permission denied"
        for words in (("mv", b, w / "b2"), ("-n", "mv", b, w / "b2"),
                      ("cp", b, w / "c"), ("-n", "cp", b, w / "c")):
            done = run(home, *words)
            assert done.returncode == 1 and done.stdout == "" and said in done.stderr, \
                (words, done)
        renamed = run(home, "mv", "--replace", "b", "--with", "B", "--", b)
        assert renamed.returncode == 1 and said in renamed.stderr, renamed
    finally:
        for path in w.iterdir():
            path.chmod(0o644)
    assert sorted(p.name for p in w.iterdir()) == ["b", "other"]
    assert os.getxattr(b, "user.omafiles.tags") == b"t,extra"


def check_r9_a_kept_file_not_made_yet_is_named_as_a_place(data, workspace):
    """A directory where omafiles will keep a file it has not made yet is
    refused as that place, not as holding the file."""
    home, w = fresh(workspace, "r9-kept-place")
    index = home / "omafiles" / "index.jsonl"
    done = run(home, "--yes", "rm", "--", home)
    assert done.returncode == 1, done
    assert f"rm: refusing {home}: it is where omafiles keeps {index}\n" in done.stderr, done
    assert run(home, "tag", "add", "-t", "x", "--", w).returncode == 0
    done = run(home, "--yes", "rm", "--", home)
    assert f"rm: refusing {home}: it holds {index}, which omafiles keeps\n" in done.stderr, done


def check_r9_a_link_moves_without_the_row_at_its_name(data, workspace):
    """A symlink standing where a tagged file was carries none of that file's
    row: moving it drops the row with a note, in a rename of a set as well."""
    home, w = fresh(workspace, "r9-link-row")
    for name in ("f", "g", "h"):
        (w / name).write_text(name)
    assert run(home, "tag", "add", "-t", "ff", "--", w / "f", w / "h").returncode == 0
    assert run(home, "tag", "add", "-t", "gg", "--", w / "g").returncode == 0
    for name in ("f", "h"):
        (w / name).unlink()
        (w / name).symlink_to("g")
    note = "had a row for a file that is gone; moving the link {} it and its tags ff\n"
    dry = run(home, "--dry-run", "--json", "mv", "--", w / "f", w / "f2")
    assert dry.returncode == 0 and json.loads(dry.stdout)["rows"] == [], dry
    assert dry.stderr == f"mv: {w}/f " + note.format("would drop"), dry
    moved = run(home, "--json", "mv", "--", w / "f", w / "f2")
    assert moved.returncode == 0 and json.loads(moved.stdout)["rows"] == [], moved
    assert moved.stderr == f"mv: {w}/f " + note.format("drops"), moved
    renamed = run(home, "mv", "--replace", "h", "--with", "h2", "--", w / "h")
    assert renamed.returncode == 0 and renamed.stderr == f"mv: {w}/h " + note.format("drops"), \
        renamed
    rows = [json.loads(line) for line in run(home, "export").stdout.splitlines()]
    assert [(row["path"], row["tags"]) for row in rows] == [(f"{w}/g", ["gg"])], rows


def check_r9_an_unset_home_is_named_unset(data, workspace):
    """`HOME` unset where it decides a base directory is said to be unset."""
    home = workspace / "r9-unset-home"
    home.mkdir()
    env = {k: v for k, v in cli_env(home).items() if k not in ("HOME", "XDG_DATA_HOME")}
    done = subprocess.run([str(CLI), "tags"], env=env, cwd=str(ROOT), text=True,
                          stdin=subprocess.DEVNULL, capture_output=True)
    assert done.returncode == 1, done
    assert done.stderr.startswith("omafiles: HOME is not set; "), done


def check_r9_a_list_that_will_not_go_is_named(data, workspace):
    """`roots rm` names a list of names it could not remove, exit 3, and `roots
    add` of that id refuses until it goes, so no root answers from another's."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r9-stuck-list")
    (w / "in").mkdir()
    (w / "in" / "a").write_text("a")
    assert run(home, "roots", "add", "--id", "in", "--", w / "in").returncode == 0
    assert run(home, "sweep").returncode == 0
    names = home / "cache" / "omafiles" / "names"
    said = f"cannot remove {names}/in: Permission denied\n"
    names.chmod(0o500)
    try:
        removed = run(home, "roots", "rm", "--id", "in")
        assert removed.returncode == 3 and removed.stderr == f"roots rm: {said}", removed
        added = run(home, "roots", "add", "--id", "in", "--", w / "in")
        assert added.returncode == 3 and added.stderr == f"roots add: {said}", added
        assert run(home, "roots").stdout == ""
    finally:
        names.chmod(0o700)
    assert run(home, "roots", "add", "--id", "in", "--", w / "in").returncode == 0
    assert not (names / "in").exists()


def check_r9_an_unplugged_mount_point_may_be_declared(data, workspace):
    """`roots add --mount` takes a mount point that is not there, as udisks
    leaves one unplugged, named as `realpath -m` names it, and reads `away`; a
    `..` after the part that is there, a dangling link, or no `--mount` is
    still a path that is not there."""
    home, w = fresh(workspace, "r9-unplugged")
    (w / "media").mkdir()
    (w / "media" / "L").symlink_to("nowhere")
    added = run(home, "roots", "add", "--mount", "--id", "stick", "--", "media/STICK/",
                cwd=w)
    assert added.returncode == 0 and added.stdout == f"{w}/media/STICK\n", added
    assert run(home, "roots").stdout == f"stick\t{w}/media/STICK\taway\n"
    for words in (("--mount", "--id", "b", "--", "media/X/../Y"),
                  ("--mount", "--id", "c", "--", "media/L"),
                  ("--id", "d", "--", "media/NOPE")):
        done = run(home, "roots", "add", *words, cwd=w)
        assert done.returncode == 2 and done.stdout == "", (words, done)


def check_r9_a_batch_names_the_rows_it_took_away(data, workspace):
    """A row there before the batch and gone after it is in the receipt with
    empty `tags`, as `tag clear` prints it on its own, and in the dry run's
    paths; a path with no row before or after is in neither."""
    home, w = fresh(workspace, "r9-batch-gone")
    cleared, emptied, pruned, passing = w / "cleared", w / "emptied", w / "pruned", w / "passing"
    for path in (cleared, emptied, pruned, passing):
        path.write_text(path.name)
    assert run(home, "tag", "add", "-t", "a", "-t", "b", "--", cleared, emptied, pruned) \
        .returncode == 0
    pruned.unlink()
    lines = (f"tag clear -- {cleared}\ntag rm -t a -- {emptied}\ntag rm -t b -- {emptied}\n"
             f"tag add -t c -- {passing}\ntag clear -- {passing}\nprune --all\n")
    gone = [str(cleared), str(emptied), str(pruned)]
    dry = run(home, "-y", "-n", "batch", stdin=lines)
    assert dry.returncode == 0 and dry.stdout.splitlines() == gone, dry
    done = run(home, "-y", "--json", "batch", stdin=lines)
    assert done.returncode == 0, done
    rows = json.loads(done.stdout)
    assert [row["path"] for row in rows] == gone, rows
    assert all(row["tags"] == [] and row["tagged"] > 0 for row in rows), rows


def check_r9_a_batch_line_escapes_in_double_quotes_as_sh(data, workspace):
    """Inside double quotes a backslash escapes `$`, a backtick, `"` and itself,
    as POSIX 2.2.3 has it, and stays before anything else."""
    home, w = fresh(workspace, "r9-batch-dquote")
    names = ["a$b", "a`b", 'a"b', "a\\b", "a\\nb"]
    for name in names:
        (w / name).write_text("x")
    line = 'tag add -t t -- "a\\$b" "a\\`b" "a\\"b" "a\\\\b" "a\\nb"\n'
    done = run(home, "--json", "batch", stdin=line, cwd=w)
    assert done.returncode == 0, done
    assert sorted(row["path"] for row in json.loads(done.stdout)) \
        == sorted(str(w / name) for name in names), done.stdout


def check_r9_the_bash_shim_matches_the_word_as_the_shell_reads_it(data, workspace):
    """The word under the cursor is matched with its quotes undone and with the
    pieces COMP_WORDBREAKS cut at `=` and `:` joined again; the reply is what
    readline puts in place of its own piece."""
    bash = shutil.which("bash")
    if not bash:
        return
    home, w = fresh(workspace, "r9-bash-word")
    for name in ("key=value.txt", "a:b.txt"):
        (w / name).write_text("x")
    assert run(home, "view", "add", "--name", "my view", "--tag", "x").returncode == 0
    assert run(home, "view", "add", "--name", "myv", "--tag", "x").returncode == 0
    script = run(home, "completions", "bash").stdout
    probe = script + 'omafiles() { "$OMAFILES" "$@"; }\n' + "".join(
        f"COMP_WORDS=({words}); COMP_CWORD={cword}; _omafiles omafiles; "
        'printf "[%s]" "${COMPREPLY[@]}"; echo\n'
        for words, cword in (("omafiles view rm --name 'my\\ v'", 4),
                             ("omafiles view rm --name \"'my v\"", 4),
                             ("omafiles view rm --name '\"my v'", 4),
                             ("omafiles view rm --name '\"my\\ v'", 4),
                             ("omafiles view rm --name = 'my\\ v'", 5),
                             ("omafiles open key = v", 4),
                             ("omafiles json a : b", 4),
                             ("omafiles json a :", 3),
                             ("omafiles mv a : b.txt ''", 5)))
    done = subprocess.run([bash, "-c", probe], env={**cli_env(home), "OMAFILES": str(CLI)},
                          cwd=str(w), text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
    assert done.stdout.splitlines() == ["[my\\ view]", "[my view]", "[my view]", "[]",
                                        "[my\\ view]",
                                        "[value.txt]", "[b.txt]", "[b.txt]",
                                        "[a:b.txt][key=value.txt]"], done


def check_r9_a_batch_calls_a_listing_a_read(data, workspace):
    """`view ls` and `roots ls`, spelled or not, are refused as reads."""
    home, _ = fresh(workspace, "r9-batch-listing")
    for line, named in (("view ls", "view ls"), ("view", "view ls"), ("roots", "roots ls"),
                        ("roots ls", "roots ls"), ("tag ls x", "tag ls")):
        refused = run(home, "batch", stdin=line + "\n")
        assert refused.returncode == 1, (line, refused)
        assert f"line 1: {named} only reads" in refused.stderr, (line, refused.stderr)
    written = run(home, "batch", stdin="view add --name v -t x\n")
    assert "view writes a file" in written.stderr, written.stderr


def check_r9_reindex_names_hard_links_tagged_apart(data, workspace):
    """Two names of one file whose rows disagree cannot both win: plain
    `reindex` says so, writes neither copy and exits 1 every run, and
    `--from-xattr` settles them."""
    home, w = fresh(workspace, "r9-reindex-links")
    if not carries_xattrs(w):
        return
    a, b = w / "a", w / "b"
    a.write_text("a")
    os.link(a, b)
    assert run(home, "tag", "add", "-t", "x", "--", a).returncode == 0
    os.removexattr(a, "user.omafiles.tags")
    assert run(home, "tag", "add", "-t", "y", "--", b).returncode == 0
    for _ in range(2):
        torn = run(home, "reindex", w)
        assert torn.returncode == 1 and torn.stdout == "", torn
        assert f"{a}, {b} are one file whose rows hold different tags" in torn.stderr, torn
        assert os.getxattr(a, "user.omafiles.tags") == b"y"
    assert run(home, "reindex", "--from-xattr", w).returncode == 0
    assert run(home, "reindex", w).returncode == 0
    assert run(home, "verify", w).returncode == 0


def check_r9_top_level_refusals_name_the_program(data, workspace):
    """A refusal made before any verb is named says whose it is."""
    for words, said in ((("-yn", "tags"), "omafiles: -yn is not a global flag"),
                        (("frob",), "omafiles: unknown command: frob"),
                        (("frob", "--help"), "omafiles: unknown command: frob")):
        refused = run(data, *words)
        assert refused.returncode == 1 and refused.stderr.startswith(said), (words, refused)


def check_r9_view_add_names_itself(data, workspace):
    """`view add`'s refusals start `view add:`, as `view rm`'s start `view rm:`."""
    home, w = fresh(workspace, "r9-view-add")
    for words in (("--name", "a/b", "-t", "x"), ("--name", "v", "-t", "Bad Tag")):
        refused = run(home, "view", "add", *words)
        assert refused.returncode == 1 and refused.stderr.startswith("view add: "), refused


def check_r9_tags_says_its_notes_in_the_order_ls_does(data, workspace):
    """The tree it could not open first, then the rows under it."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r9-tags-notes")
    shut = w / "shut"
    shut.mkdir()
    (shut / "x").write_text("x")
    assert run(home, "tag", "add", "-t", "a", "--", shut / "x").returncode == 0
    shut.chmod(0)
    try:
        said = run(home, "tags", shut)
    finally:
        shut.chmod(0o700)
    assert said.returncode == 1, said
    assert said.stderr.splitlines() == [
        f"tags: {shut} is unreadable; nothing under it was counted: Permission denied",
        f"tags: {shut / 'x'}: Permission denied; the row was not counted"], said.stderr


def check_r9_import_says_nothing_of_a_merge_it_refused(data, workspace):
    """A dump a later line refuses merges nothing, and no earlier line's note
    about a copy not written is said for it."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r9-import-notes")
    shut, closed = w / "shut", w / "closed"
    shut.mkdir()
    (shut / "x").write_text("x")
    closed.write_text("c")
    dump = w / "dump.jsonl"
    dump.write_text(json.dumps({"path": str(shut / "x"), "tags": ["a"]}) + "\n"
                    + json.dumps({"path": str(closed), "tags": ["b"]}) + "\n")
    alone = w / "alone.jsonl"
    alone.write_text(dump.read_text().splitlines()[0] + "\n")
    shut.chmod(0)
    closed.chmod(0)
    try:
        refused = run(home, "import", dump)
        merged = run(home, "import", alone)
    finally:
        shut.chmod(0o700)
        closed.chmod(0o600)
    assert refused.returncode == 1, refused
    assert "was not written" not in refused.stderr, refused.stderr
    assert "nothing was written" in refused.stderr, refused.stderr
    assert merged.returncode == 0, merged
    assert "its own copy of the tags was not written" in merged.stderr, merged.stderr


def check_r10_an_alias_the_directory_resolves_is_the_stored_name(data, workspace):
    """On a directory that folds case, a name in another normalization reaches
    the stored entry and is that entry's name, as an 8.3 alias or a trailing
    dot is on vfat: the tags land on one row, every read finds them by either
    spelling, and `mv` by the alias takes the row along. `strasse` reaches
    `straße` by full case folding, which no key here spells, so only the
    file's identity tells it."""
    home, w = fresh(workspace, "r10-alias")
    cf = w / "cf"
    cf.mkdir()
    nfc, nfd = "caf\u00e9", "cafe\u0301"
    done = in_mounts(home, CASEFOLD.format(cf), f"""
cd {cf}
echo x > {nfc}
"$CLI" tag add -t t {nfd} >/dev/null
"$CLI" tag add -t u {nfc} >/dev/null
"$CLI" export
"$CLI" has -t u {nfd} && echo has
"$CLI" mv {nfd} moved >/dev/null
echo s > straße
"$CLI" tag add -t s strasse >/dev/null
"$CLI" export
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    assert [(json.loads(line)["path"], json.loads(line)["tags"]) for line in lines[:1]] \
        == [(f"{cf}/{nfc}", ["t", "u"])] and lines[1] == "has", done
    assert [(json.loads(line)["path"], json.loads(line)["tags"]) for line in lines[2:]] \
        == [(f"{cf}/moved", ["t", "u"]), (f"{cf}/straße", ["s"])], done


def check_r10_a_list_of_names_finds_a_recased_row(data, workspace):
    """A name the list of names holds in the case a folding directory had
    before another program re-cased it is that file's: `ls --untagged
    --cached` does not call a tagged file untagged, and `--name` carries its
    tags in the spelling the directory holds, whether the row was written
    before the re-case or after it, for a re-cased folder, and for a name
    another program respelled in another normalization."""
    home, w = fresh(workspace, "r10-recased-list")
    cf = w / "cf"
    cf.mkdir()
    nfc, nfd = "caf\u00e9", "cafe\u0301"
    done = in_mounts(home, CASEFOLD.format(cf), f"""
cd {cf}
mkdir Photos; echo x > foo.txt; echo y > other.txt; echo z > bar.txt; echo p > Photos/p1.jpg
echo c > {nfd}
"$CLI" roots add --id usb {cf} >/dev/null
"$CLI" tag add -t usb foo.txt Photos/p1.jpg {nfd} >/dev/null
"$CLI" sweep >/dev/null 2>&1
mv foo.txt t; mv t Foo.TXT; mv bar.txt t; mv t BAR.txt; mv Photos t; mv t photos
mv {nfd} t; mv t {nfc}
"$CLI" tag add -t b BAR.txt >/dev/null
"$CLI" ls --untagged --cached 2>/dev/null
"$CLI" --json ls --name '*' --cached 2>/dev/null
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    assert lines[:2] == [f"{cf}/Photos", f"{cf}/other.txt"], done
    rows = {row["path"]: row["tags"] for row in json.loads(lines[2])}
    assert rows == {f"{cf}/BAR.txt": ["b"], f"{cf}/Foo.TXT": ["usb"], f"{cf}/Photos": [],
                    f"{cf}/photos/p1.jpg": ["usb"], f"{cf}/other.txt": [],
                    f"{cf}/{nfc}": ["usb"]}, done


def check_r10_a_kept_file_not_made_yet_is_refused_in_another_case(data, workspace):
    """Where the directory omafiles keeps a file in folds case, a name one
    case apart from a kept file not made yet is that file: `mv` and `cp` onto
    it are refused, and every verb still reads."""
    home, w = fresh(workspace, "r10-kept-casefold")
    config = home / "config"
    config.mkdir()
    kept = config / "omafiles"
    done = in_mounts(home, CASEFOLD.format(config), f"""
cd {w}
"$CLI" view add --name v -t a >/dev/null
echo j > j
"$CLI" mv j {kept}/ROOTS.jsonl; echo "mv $?"
"$CLI" cp j {kept}/Roots.jsonl; echo "cp $?"
"$CLI" tags; echo "tags $?"
ls {kept}
""")
    if done is None:
        return
    assert done.stdout.splitlines() == ["mv 1", "cp 1", "tags 0", "views.jsonl"], done
    for verb, name in (("mv", "ROOTS.jsonl"), ("cp", "Roots.jsonl")):
        assert f"{verb}: refusing {kept}/{name}: omafiles keeps it" in done.stderr, done.stderr


def check_r10_a_walk_names_an_away_root_whose_mount_point_is_gone(data, workspace):
    """A declared root that is away under a walked tree is named whether its
    mount point is there or udisks took it away, and the walk exits 1 either
    way; `sweep` says so too. A root declared inside it is not named again."""
    home, w = fresh(workspace, "r10-away-gone")
    media = w / "media"
    media.mkdir()
    (media / "f").write_text("a")
    for root, path in (("in", media / "STICK" / "in"), ("s", media / "STICK")):
        assert run(home, "roots", "add", "--mount", "--id", root, "--", path).returncode == 0
    said = f"{media}/STICK: root s is declared and not here; nothing under it was"
    for present in (True, False):
        if present:
            (media / "STICK").mkdir()
        walked = run(home, "ls", "--untagged", media)
        assert walked.returncode == 1 and walked.stderr == f"ls: {said} listed\n", walked
        if present:
            (media / "STICK").rmdir()
    assert run(home, "roots", "add", "--id", "m", "--", media).returncode == 0
    swept = run(home, "sweep", media)
    assert f"sweep: {said} swept" in swept.stderr, swept


def check_r10_a_batch_names_a_value_it_emptied(data, workspace):
    """A file with no row whose own value a batch empties is in `--json` with no
    tags, stamped with the drop, and in `--dry-run`, as on its own; a path whose
    row and value end the run as they began is in neither."""
    home, w = fresh(workspace, "r10-batch-value")
    if not carries_xattrs(w):
        return
    for name in ("c2", "c3", "c4", "c5"):
        (w / name).write_text("x")
        os.setxattr(w / name, "user.omafiles.tags", b"k1,k2")
    (w / "plain").write_text("p")
    listed = run(home, "--json", "batch", "-", stdin="tag clear -- c3\n", cwd=w)
    assert listed.returncode == 0, listed
    assert [{**row, "tagged": row.get("tagged", 0) > 0} for row in json.loads(listed.stdout)] \
        == [{"path": str(w / "c3"), "tags": [], "tagged": True}], listed.stdout
    assert "user.omafiles.tags" not in os.listxattr(w / "c3")
    previewed = run(home, "--dry-run", "batch", "-", stdin="tag clear -- c4\n", cwd=w)
    assert previewed.returncode == 0 and previewed.stdout == f"{w / 'c4'}\n", previewed
    assert run(home, "--dry-run", "tag", "clear", "--", "c2", cwd=w).stdout == f"{w / 'c2'}\n"
    emptied = run(home, "--json", "batch", "-", stdin="tag rm -t k1 -t k2 -- c5\n", cwd=w)
    assert [{**row, "tagged": row.get("tagged", 0) > 0} for row in json.loads(emptied.stdout)] \
        == [{"path": str(w / "c5"), "tags": [], "tagged": True}], emptied.stdout
    round_trip = "tag add -t q -- plain\ntag rm -t q -- plain\n"
    assert run(home, "--json", "batch", "-", stdin=round_trip, cwd=w).stdout == "[]\n"
    assert run(home, "--dry-run", "batch", "-", stdin=round_trip, cwd=w).stdout == ""


def check_r10_a_row_in_the_other_form_is_the_same_row(data, workspace):
    """A row is its file, tags and stamp: an import spelling it absolute where
    the index holds it under a root, or the other way round, changes nothing and
    lists nothing, while `roots migrate` and `roots rm` still rewrite the form."""
    home, w = fresh(workspace, "r10-row-form")
    tree = w / "r1b"
    tree.mkdir()
    (tree / "top").write_text("x")
    index = home / "omafiles" / "index.jsonl"
    absolute = {"path": str(tree / "top"), "tags": ["a"]}
    rooted = {"root": "r1", "path": "top", "tags": ["a"]}
    for migrated in (False, True):
        shutil.rmtree(home / "omafiles", ignore_errors=True)
        shutil.rmtree(home / "config", ignore_errors=True)
        assert run(home, "tag", "add", "-t", "a", "--", tree / "top").returncode == 0
        assert run(home, "roots", "add", "--id", "r1", "--", tree).returncode == 0
        if migrated:
            assert run(home, "roots", "migrate").returncode == 0
            assert json.loads(index.read_text())["root"] == "r1", index.read_text()
        before = index.read_bytes()
        for row in (absolute, rooted):
            dump = json.dumps(row) + "\n"
            listed = run(home, "--json", "import", "-", stdin=dump)
            assert listed.returncode == 0 and listed.stdout == "[]\n", (migrated, row, listed)
            assert run(home, "--dry-run", "import", "-", stdin=dump).stdout == "", (migrated, row)
            assert index.read_bytes() == before, (migrated, row, index.read_text())
    assert run(home, "roots", "rm", "--id", "r1").returncode == 0
    assert json.loads(index.read_text())["path"] == str(tree / "top"), index.read_text()


def check_r10_a_dump_names_the_declaration_its_root_needs(data, workspace):
    """A dump row under a root not declared here is refused with the command
    that declares it, not an index repair."""
    home, _ = fresh(workspace, "r10-dump-root")
    refused = run(home, "import", "-", stdin=json.dumps(
        {"path": "x/y", "root": "nosuchroot", "tags": ["a"]}) + "\n")
    assert refused.returncode == 1, refused
    assert refused.stderr == (
        "import: line 1: index row names the root nosuchroot, which is not declared here; the "
        "rows under it are placed once `omafiles roots add --id nosuchroot <dir>` declares "
        "it\n"), refused.stderr


def check_r10_bash_ends_a_folder_past_a_word_break_with_a_slash(data, workspace):
    """Over a real terminal, Tab ends a folder with `/` and a file with a space,
    past a `:` or `=`, with a space or a quote in the name, and inside an open
    quote; a prefix two folders share gets neither."""
    bash = shutil.which("bash")
    if not bash:
        return
    home, w = fresh(workspace, "r10-bash-dirs")
    for folder in ("d:1", "e=1", "h s", "j'q", "m:n o", "pdir", "s:ab1", "s:ab2"):
        (w / folder).mkdir()
    for name in ("f:1.txt", "g=1.txt", "i s.txt", 'k"q.txt', "r:s t.txt"):
        (w / name).write_text("x")
    shim = home / "shim.bash"
    shim.write_text(run(home, "completions", "bash").stdout
                    + 'omafiles() { "$OMAFILES" "$@"; }\n'
                    + """bind -x '"\\C-t": printf "\\n<%s>\\n" "$READLINE_LINE"'\n""")
    cases = (("d:", "d:1/"), ("e=", "e=1/"), ("f:", "f:1.txt "), ("g=", "g=1.txt "),
             ("h", "h\\ s/"), ("i", "i\\ s.txt "), ("j", "j\\'q/"), ("k", 'k\\"q.txt '),
             ('"h', '"h s"/'), ('"i', '"i s.txt" '), ("m:", "m:n\\ o/"),
             ("r:", "r:s\\ t.txt "), ("'m:", "'m:n o'/"), ("pd", "pdir/"), ("s:a", "s:ab"))
    parent, child = pty.openpty()
    shell = subprocess.Popen(
        [bash, "--norc", "--noprofile", "-i"], cwd=str(w), stdin=child, stdout=child,
        stderr=child, start_new_session=True,
        env={**cli_env(home), "OMAFILES": str(CLI), "INPUTRC": "/dev/null", "TERM": "dumb"})
    os.close(child)

    def until(pattern):
        seen = b""
        while not re.search(pattern, seen):
            ready, _, _ = select.select([parent], [], [], 20)
            if not ready:
                break
            try:
                chunk = os.read(parent, 4096)
            except OSError:
                break
            if not chunk:
                break
            seen += chunk
        return seen

    try:
        os.write(parent, f"PS1='$ '; . '{shim}'; echo READY\n".encode())
        until(rb"READY\r?\n\$ ")
        got = []
        for typed, _ in cases:
            # Tab, then C-t to print the line, then C-u to clear it.
            os.write(parent, f"omafiles json {typed}\t\x14\x15".encode())
            line = re.findall(rb"\n<([^\n]*)>\r?\n", until(rb"\n<[^\n]*>\r?\n"))
            got.append(line[-1].decode() if line else None)
        os.write(parent, b"exit\n")
        shell.wait(timeout=20)
    finally:
        if shell.poll() is None:
            shell.kill()
        os.close(parent)
    assert got == [f"omafiles json {line}" for _, line in cases], got


def check_r10_completion_has_no_opinion_past_a_word_that_is_no_global(data, workspace):
    """A word in the verb's place that the parse refuses -- a flag no global is,
    a lone `-`, a word that is no verb -- leaves completion with no opinion, as a
    verb's unknown flag does."""
    home, _ = fresh(workspace, "r10-complete-global")
    for words in (("--bogus", ""), ("--bogus", "-"), ("-j", "--bogus", ""), ("-", ""),
                  ("--tag", "x", ""), ("frob", "-")):
        offered = run(home, "__complete", "--", *words)
        assert offered.returncode == 1 and offered.stdout == "", (words, offered)
    offered = run(home, "__complete", "--", "-j", "")
    assert offered.returncode == 0 and "tag" in offered.stdout.split(), offered


def check_r10_a_copy_that_cannot_carry_a_foreign_word_is_refused(data, workspace):
    """Onto a filesystem that holds no value, a file whose value carries a word
    its row does not record is refused by `mv` and `cp`, and keeps its value;
    one whose row records every word goes, the row the record."""
    home, w = fresh(workspace, "r10-foreign-word")
    if not carries_xattrs(w):
        return
    rf = w / "rf"
    rf.mkdir()
    for name in ("mixed", "clean"):
        (w / name).write_text(name)
        assert run(home, "tag", "add", "-t", "good", "--", w / name).returncode == 0
    os.setxattr(w / "mixed", "user.omafiles.tags", b"good,Bad W")
    done = in_mounts(home, f"mount -t ramfs ramfs {rf}", f"""
"$CLI" mv {w}/mixed {rf}/mixed 2>&1; echo "mv $?"
"$CLI" cp {w}/mixed {rf}/copied 2>&1; echo "cp $?"
"$CLI" mv {w}/clean {rf}/clean 2>&1 >/dev/null; echo "clean $?"
ls {rf}
""")
    if done is None:
        return
    lines = done.stdout.splitlines()
    said = "the copy refused the words (Bad W), which no row records"
    assert lines[0].startswith("mv: cannot move") and said in lines[0], done
    assert lines[1:3] == ["mv 1", lines[2]] and said in lines[2], done
    assert lines[3:] == ["cp 1", "clean 0", "clean"], done
    assert os.getxattr(w / "mixed", "user.omafiles.tags") == b"good,Bad W"


def check_r10_tag_merge_names_a_row_a_link_now_leads_through(data, workspace):
    """A row whose path is now a symlink is renamed in its row, written through
    nowhere, and named in a note; a row whose file is gone is not."""
    home, w = fresh(workspace, "r10-merge-link")
    for name in ("a", "b", "c"):
        (w / name).write_text(name)
    assert run(home, "tag", "add", "-t", "old", "--", w / "a", w / "b", w / "c").returncode == 0
    (w / "a").unlink()
    (w / "a").symlink_to("b")
    (w / "c").unlink()
    merged = run(home, "tag", "merge", "old", "new")
    assert merged.returncode == 0, merged
    assert merged.stderr == (f"tag merge: {w / 'a'} leads through a symlink to {w / 'b'} now; "
                             "its xattr was not written\n"), merged.stderr
    assert run(home, "tag", "ls", "--", w / "b").stdout == "new\n"


def check_r10_mv_into_a_link_to_itself_is_inside_itself(data, workspace):
    """`mv nd L/` with `L` a link to `nd` is a move into itself, as `mv nd L` is
    and as GNU mv says; a trailing `/` does not make it a rename onto `L`."""
    home, w = fresh(workspace, "r10-mv-link-slash")
    (w / "nd").mkdir()
    (w / "L").symlink_to("nd")
    for typed in ("L/", "L"):
        refused = run(home, "mv", "nd", typed, cwd=w)
        assert refused.returncode == 1, refused
        assert refused.stderr == f"mv: {w / 'nd' / 'nd'} is inside {w / 'nd'}\n", refused.stderr
    assert (w / "nd").is_dir() and (w / "L").is_symlink()


def check_r11_prune_takes_the_tree_a_move_left(data, workspace):
    """`prune` of a folder or a file another program moved away drops the rows
    at it and under it, as the `mv` help tells a user to; a gone path with no
    rows is still `path missing`, exit 2."""
    home, w = fresh(workspace, "r11-prune-gone")
    (w / "src").mkdir()
    (w / "src" / "a").write_text("a")
    (w / "f1").write_text("f")
    (w / "kept").write_text("k")
    assert run(home, "tag", "add", "-t", "x", "--", w / "src" / "a", w / "f1", w / "kept") \
        .returncode == 0
    (w / "src").rename(w / "dst")
    (w / "f1").rename(w / "f2")
    previewed = run(home, "-n", "prune", "src", cwd=w)
    assert previewed.returncode == 0 and previewed.stdout == f"{w / 'src' / 'a'}\n", previewed
    for gone, row in (("src", w / "src" / "a"), ("f1", w / "f1")):
        dropped = run(home, "-y", "--json", "prune", gone, cwd=w)
        assert dropped.returncode == 0, dropped
        assert json.loads(dropped.stdout) == {"dropped": [str(row)]}, dropped.stdout
    assert [json.loads(line)["path"] for line in run(home, "export").stdout.splitlines()] \
        == [str(w / "kept")]
    for never in ("never-was", "src"):
        refused = run(home, "-y", "prune", never, cwd=w)
        assert refused.returncode == 2 and refused.stderr == f"prune: path missing: {never}\n", \
            refused


def check_r11_a_list_of_names_is_short_where_its_root_is_shut(data, workspace):
    """A root whose own directory `sweep` cannot open gets a list that says so:
    `ls --untagged --cached` names the gap and exits 1, not an empty answer."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r11-names-root-shut")
    top = w / "Z0"
    (top / "d").mkdir(parents=True)
    (top / "d" / "f").write_text("z")
    assert run(home, "roots", "add", "--id", "z0", "--", top).returncode == 0
    top.chmod(0)
    try:
        assert run(home, "sweep").returncode == 0
        for scope in ((top,), ()):
            cached = run(home, "ls", "--untagged", "--cached", *scope)
            assert cached.returncode == 1 and cached.stdout == "", (scope, cached)
            assert f"the list of names is short: {top} is unreadable" in cached.stderr, cached
    finally:
        top.chmod(0o755)


def check_r11_roots_migrate_writes_only_a_change(data, workspace):
    """`roots migrate` over rows already in their form leaves the index file as
    it is, and makes none where there was none."""
    home, w = fresh(workspace, "r11-migrate-noop")
    (w / "r").mkdir()
    (w / "r" / "a").write_text("a")
    assert run(home, "tag", "add", "-t", "x", "--", w / "r" / "a").returncode == 0
    assert run(home, "roots", "add", "--id", "r", "--", w / "r").returncode == 0
    index = home / "omafiles" / "index.jsonl"
    assert run(home, "roots", "migrate").returncode == 0
    before = index.stat().st_ino
    again = run(home, "roots", "migrate")
    assert again.returncode == 0 and index.stat().st_ino == before, again
    empty, room = fresh(workspace, "r11-migrate-none")
    assert run(empty, "roots", "add", "--id", "r", "--", room).returncode == 0
    assert run(empty, "roots", "migrate").returncode == 0
    assert not (empty / "omafiles" / "index.jsonl").exists()


def check_r11_a_sweep_that_steps_aside_counts_an_away_root_inside(data, workspace):
    """A sweep that steps aside for another writer before a tree still counts
    the rows under an away root declared inside it as skipped, as a run that
    swept the tree does."""
    home, w = fresh(workspace, "r11-sweep-aside")
    media = w / "media"
    media.mkdir()
    assert run(home, "roots", "add", "--id", "m", "--", media).returncode == 0
    assert run(home, "roots", "add", "--mount", "--id", "s", "--", media / "STICK").returncode == 0
    write_index(home, [{"path": str(media / "STICK" / "g"), "tags": ["x"], "tagged": 1}])
    counts = []
    for held in (False, True):
        lock = forge_lock(home) if held else None
        try:
            swept = run(home, "--json", "sweep")
        finally:
            if lock:
                lock.unlink(missing_ok=True)
        assert swept.returncode == 0, swept
        assert ("stepped aside" in swept.stderr) == held, swept
        counts.append(json.loads(swept.stdout)["skipped"])
    assert counts == [1, 1], counts


def check_r11_bash_completes_a_path_attached_to_a_flag(data, workspace):
    """`--from=<partial>` completes files for the part after the `=`, and a tag
    typed in another case reaches the folded tag."""
    bash = shutil.which("bash")
    if not bash:
        return
    home, w = fresh(workspace, "r11-bash-attached")
    (w / "my file.txt").write_text("x")
    (w / "dd").mkdir()
    (w / "t").write_text("t")
    assert run(home, "tag", "add", "-t", "work", "--", w / "t").returncode == 0
    script = run(home, "completions", "bash").stdout
    probe = script + 'omafiles() { "$OMAFILES" "$@"; }\n' + "".join(
        f"COMP_WORDS=({words}); COMP_CWORD={cword}; _omafiles omafiles; "
        'printf "[%s]" "${COMPREPLY[@]}"; echo\n'
        for words, cword in (("omafiles tag cp --from = my", 5),
                             ("omafiles tag cp --from = d", 5),
                             ("omafiles tag cp --from my", 4),
                             ("omafiles tag add -t Wo", 4),
                             ("omafiles tag add --tag = WO", 5)))
    done = subprocess.run([bash, "-c", probe], env={**cli_env(home), "OMAFILES": str(CLI)},
                          cwd=str(w), text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
    assert done.stdout.splitlines() == ["[my file.txt]", "[dd/]", "[my file.txt]", "[work]",
                                        "[work]"], done


def check_r11_a_batch_stamps_the_row_it_emptied_with_the_drop(data, workspace):
    """The receipt of a row a batch line emptied carries the drop's stamp, as
    `tag clear` prints on its own, not the stamp the row was read with."""
    home, w = fresh(workspace, "r11-batch-stamp")
    for name in ("sd1", "sd2"):
        (w / name).write_text(name)
    write_index(home, [{"path": str(w / name), "tags": ["a"], "tagged": 1000}
                       for name in ("sd1", "sd2")])
    started = int(time.time())
    for line in ("tag clear -- sd1\n", "tag rm -t a -- sd2\n"):
        done = run(home, "--json", "batch", stdin=line, cwd=w)
        assert done.returncode == 0, done
        rows = json.loads(done.stdout)
        assert len(rows) == 1 and rows[0]["tags"] == [] and rows[0]["tagged"] >= started, rows


def check_r11_hard_links_tagged_apart_in_a_batch_agree_with_the_dry_run(data, workspace):
    """Two names of one file, tagged by one line and cleared by the next: the
    real receipt lists what the dry run does, and not the name whose row and
    copy end as they began."""
    home, w = fresh(workspace, "r11-batch-links")
    if not carries_xattrs(w):
        return
    (w / "u1").write_text("u")
    os.link(w / "u1", w / "u1b")
    lines = "tag add --tag a -- u1\ntag clear -- u1b\n"
    dry = run(home, "--json", "-n", "batch", stdin=lines, cwd=w)
    done = run(home, "--json", "batch", stdin=lines, cwd=w)
    assert dry.returncode == 0 and done.returncode == 0, (dry, done)
    named = [[row["path"] for row in json.loads(answer.stdout)] for answer in (dry, done)]
    assert named == [[str(w / "u1")], [str(w / "u1")]], named


def check_r11_a_destination_under_an_away_root_is_named_for_the_root(data, workspace):
    """`mv` and `cp` into an unplugged `--mount` root say the root is declared
    and not here, with a trailing `/` and with the mount point's parent gone."""
    home, w = fresh(workspace, "r11-dest-away")
    (w / "z3").write_text("x")
    media = w / "media"
    media.mkdir()
    assert run(home, "roots", "add", "--mount", "--id", "stick", "--", media / "STICK") \
        .returncode == 0
    for present in (True, False):
        if not present:
            media.rmdir()
        for verb, typed in (("mv", "media/STICK/"), ("cp", "media/STICK/"),
                            ("mv", "media/STICK/sub/"), ("mv", "media/STICK")):
            refused = run(home, "-n", verb, "z3", typed, cwd=w)
            assert refused.returncode == 1, (present, verb, typed, refused)
            assert refused.stderr.endswith("root stick is declared and not here\n"), \
                (present, verb, typed, refused.stderr)
    assert (w / "z3").exists()


def check_r11_the_state_directory_not_made_yet_is_no_destination(data, workspace):
    """`cp` and `mv` refuse to make a file where omafiles will make its state
    directory, and `rm` of a file already there says it stands where a kept
    file goes, not that it holds one."""
    home, w = fresh(workspace, "r11-state-dir")
    (w / "n").write_text("n")
    state = home / "state" / "omafiles"
    state.parent.mkdir(parents=True, exist_ok=True)
    for verb in ("cp", "mv"):
        refused = run(home, verb, "n", state, cwd=w)
        assert refused.returncode == 1 and not state.exists(), refused
        assert refused.stderr == (f"{verb}: refusing {state}: it is where omafiles keeps "
                                  f"{state / 'sweep.json'}\n"), refused.stderr
    state.write_text("x")
    refused = run(home, "-y", "rm", state)
    assert refused.returncode == 1 and "it is where omafiles keeps" in refused.stderr, refused


def check_r11_a_shut_parent_is_the_directory_named(data, workspace):
    """Standing in a folder whose parent was shut after `cd`, a walk and an
    answer out of the index name the parent that refuses search."""
    if os.geteuid() == 0:
        return
    home, w = fresh(workspace, "r11-shut-parent")
    inner = w / "noperm" / "in"
    inner.mkdir(parents=True)
    (inner / "f").write_text("f")
    shut = w / "noperm"
    said = []
    try:
        for words in (("ls", "--untagged"), ("tags", ".")):
            done = subprocess.run(
                ["sh", "-c", 'cd "$0" && chmod 0 "$1" && cli=$2 && shift 2 && exec "$cli" "$@"',
                 str(inner), str(shut),
                 str(CLI), *words], env=cli_env(home), text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
            shut.chmod(0o755)
            said.append((done.returncode, done.stderr.split(" is unreadable")[0]))
    finally:
        shut.chmod(0o755)
    assert said == [(1, f"ls: {shut}"), (1, f"tags: {shut}")], said


def check_r11_a_batch_names_a_refused_copy_by_its_line(data, workspace):
    """A copy the commit could not write is named by the verb of the line that
    queued it, as that line's other notes are; a line asking for help is told
    it cannot, and a flag with no value given one says so."""
    home, w = fresh(workspace, "r11-batch-words")
    (w / "t").mkdir()
    (w / "t" / "a").write_text("a")
    if carries_xattrs(w):
        os.setxattr(w / "t" / "a", "user.omafiles.tags", b"good,Bad Tag")
        done = run(home, "batch", stdin="reindex t\n", cwd=w)
        assert done.returncode == 1, done
        assert done.stderr.startswith(f"reindex: {w / 't' / 'a'} carries words that are not "
                                      "tags"), done.stderr
    helped = run(home, "batch", stdin="tag add -t a --help -- t/a\n", cwd=w)
    assert helped.returncode == 1, helped
    assert helped.stderr.startswith("batch: line 1: --help is no part of a command; a line "
                                    "cannot ask for it\n"), helped.stderr
    for words, named in ((("rm", "--yes=1", "t/a"), "rm"), (("--yes=1", "rm", "t/a"), "omafiles"),
                         (("ls", "--untagged=1"), "ls")):
        refused = run(home, *words, cwd=w)
        flag = words[words.index(next(word for word in words if "=" in word))].split("=")[0]
        assert refused.returncode == 1 and refused.stderr == \
            f"{named}: {flag} takes no value, and {flag}=1 gives it one\n", refused
    assert (w / "t" / "a").exists()


def check_r12_import_writes_nothing_through_a_link(data, workspace):
    """A dump row whose own path, a parent, or a chain of links now leads to
    another file here is stored as the dump gives it, with the note that its
    copy was not written, in a dry run and for real; the file the link leads to
    keeps its row and its value."""
    home, w = fresh(workspace, "r12-import-link")
    other = w / "other"
    other.mkdir()
    for name in ("g", "h", "z"):
        (other / name).write_text(name)
    assert run(home, "tag", "add", "-t", "mine", "--", other / "g", other / "h", other / "z") \
        .returncode == 0
    (w / "own").symlink_to("other/g")
    (w / "up").symlink_to("other")
    (w / "l2").symlink_to("other/z")
    (w / "l1").symlink_to("l2")
    for given, target in ((w / "own", other / "g"), (w / "up" / "h", other / "h"),
                          (w / "l1", other / "z")):
        dump = json.dumps({"path": str(given), "tags": ["keep"]}) + "\n"
        note = (f"import: {given} leads through a symlink to {target} now; its own copy "
                "of the tags was not written\n")
        before = run(home, "export").stdout
        dry = run(home, "-n", "--json", "import", "-", stdin=dump)
        assert dry.returncode == 0 and dry.stderr == note, (given, dry)
        assert json.loads(dry.stdout) == [{"path": str(given), "tags": ["keep"]}], dry.stdout
        assert run(home, "export").stdout == before
        done = run(home, "--json", "import", "-", stdin=dump)
        assert done.returncode == 0 and done.stderr == note, (given, done)
        assert json.loads(done.stdout) == [{"path": str(given), "tags": ["keep"]}], done.stdout
        assert run(home, "tag", "ls", "--", target).stdout == "mine\n", target
        if carries_xattrs(w):
            assert os.getxattr(target, "user.omafiles.tags") == b"mine", target
    rows = {json.loads(line)["path"] for line in run(home, "export").stdout.splitlines()}
    assert {str(w / "own"), str(w / "up" / "h"), str(w / "l1")} <= rows, rows


def check_r12_tag_writers_refuse_a_file_omafiles_keeps(data, workspace):
    """Every `tag` verb that writes refuses a file omafiles keeps, by any name,
    in the words `rm` uses, and a batch line naming one takes the batch back
    with nothing written."""
    home, w = fresh(workspace, "r12-tag-kept")
    (w / "ok").write_text("o")
    (w / "src").write_text("s")
    assert run(home, "tag", "add", "-t", "s", "--", w / "src").returncode == 0
    assert run(home, "roots", "add", "--id", "r", "--", w).returncode == 0
    assert run(home, "view", "add", "--name", "v", "--tag", "s").returncode == 0
    assert run(home, "sweep").returncode == 0
    index = home / "omafiles" / "index.jsonl"
    kept = [index, home / "config" / "omafiles" / "roots.jsonl",
            home / "config" / "omafiles" / "views.jsonl",
            home / "state" / "omafiles" / "sweep.json", home / "cache" / "omafiles" / "names" / "r"]
    assert all(path.is_file() for path in kept), [path for path in kept if not path.is_file()]
    os.link(index, w / "hard")
    for path in kept + [w / "hard"]:
        said = f"refusing {path}: omafiles keeps it\n"
        for words in (("tag", "add", "-t", "x", "--", path), ("tag", "rm", "-t", "x", "--", path),
                      ("tag", "clear", "--", path), ("tag", "cp", "--from", w / "src", "--", path),
                      ("tag", "cp", "--from", path, "--", w / "ok")):
            refused = run(home, *words)
            verb = " ".join(words[:2])
            assert refused.returncode == 1 and refused.stderr == f"{verb}: {said}", (words, refused)
    batch = run(home, "batch", stdin=f"tag add -t a -- {w / 'ok'}\ntag add -t a -- {index}\n")
    assert batch.returncode == 1, batch
    assert batch.stderr.startswith(f"tag add: refusing {index}: omafiles keeps it\n"
                                   "batch: line 2: failed, nothing was written\n"), batch.stderr
    assert run(home, "tag", "ls", "--", w / "ok").stdout == ""
    if carries_xattrs(w):
        for path in kept + [w / "ok"]:
            assert "user.omafiles.tags" not in os.listxattr(path), path


def check_r12_import_refuses_a_row_naming_a_kept_file(data, workspace):
    """A dump row naming a file omafiles keeps is an invalid row: named by its
    line, exit 1, and nothing is merged."""
    home, w = fresh(workspace, "r12-import-kept")
    (w / "a").write_text("a")
    assert run(home, "tag", "add", "-t", "t", "--", w / "a").returncode == 0
    index = home / "omafiles" / "index.jsonl"
    dump = "".join(json.dumps({"path": str(path), "tags": ["x"]}) + "\n" for path in (w / "a", index))
    before = index.read_bytes()
    for words in (("import", "-"), ("-n", "import", "-")):
        refused = run(home, *words, stdin=dump)
        assert refused.returncode == 1, refused
        assert refused.stderr == f"import: line 2: refusing {index}: omafiles keeps it\n", refused
    assert index.read_bytes() == before
    if carries_xattrs(w):
        assert os.getxattr(w / "a", "user.omafiles.tags") == b"t"


def check_r12_a_walk_skips_the_files_omafiles_keeps(data, workspace):
    """A tree holding the files omafiles keeps -- a root at the data directory's
    parent -- is walked past them, and past a link to one: `reindex` and `sweep`
    adopt nothing from them, and `ls --untagged` and `ls --name` list none."""
    home, w = fresh(workspace, "r12-walk-kept")
    if not carries_xattrs(w):
        return
    (w / "a").write_text("a")
    assert run(home, "tag", "add", "-t", "t", "--", w / "a").returncode == 0
    assert run(home, "roots", "add", "--id", "top", "--", home).returncode == 0
    assert run(home, "sweep").returncode == 0
    index = home / "omafiles" / "index.jsonl"
    kept = [index, home / "config" / "omafiles" / "roots.jsonl",
            home / "state" / "omafiles" / "sweep.json", home / "cache" / "omafiles" / "names" / "top"]
    (w / "lnk").symlink_to(index)
    for walked in (("reindex", home), ("sweep",), ("reindex", home)):
        for path in kept:
            os.setxattr(path, "user.omafiles.tags", b"evil")
        done = run(home, "--json", *walked)
        assert done.returncode == 0, (walked, done)
        assert "evil" not in run(home, "export").stdout, walked
    listed = run(home, "ls", "--untagged", home).stdout.splitlines()
    named = run(home, "ls", "--name", "*.json*", home).stdout.splitlines()
    assert not {str(path) for path in kept} & set(listed + named), (listed, named)
    assert str(w / "lnk") in listed and str(w / "a") not in listed, listed


def check_r12_a_missing_destination_is_on_the_way_to_an_away_root(data, workspace):
    """`cp` and `mv` into a folder that is not there, above an unplugged
    `--mount` root, say it is on the way to the root rather than that it holds
    it."""
    home, w = fresh(workspace, "r12-dest-on-the-way")
    (w / "t5").mkdir()
    (w / "t5" / "y").write_text("y")
    mount = w / "t5" / "usbmnt" / "x"
    assert run(home, "roots", "add", "--id", "usb", "--mount", "--", mount).returncode == 0
    for verb in ("cp", "mv"):
        refused = run(home, "-n", verb, "y", "usbmnt", cwd=w / "t5")
        assert refused.returncode == 1, (verb, refused)
        assert refused.stderr == (f"{verb}: {w / 't5' / 'usbmnt'} is on the way to {mount}: "
                                  "root usb is declared and not here\n"), refused.stderr
    assert (w / "t5" / "y").exists() and not (w / "t5" / "usbmnt").exists()


def write_receipt(home, **fields):
    receipt = {"at": 1700000000, "seconds": 1.5, "trees": ["/a"], "adopted": 0, "merged": 0,
               "gone": 0, "unreadable": 0, "skipped": 0, "listed": 0, "unlistable": 0}
    receipt.update(fields)
    path = home / "state" / "omafiles" / "sweep.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(receipt) + "\n")


def check_r12_a_receipt_count_is_the_integer_json_holds(data, workspace):
    """A count up to 2^63-1 is a count, read as the integer the receipt holds;
    2^63, a fraction and a negative are not."""
    home, _ = fresh(workspace, "r12-receipt-count")
    write_receipt(home, adopted=2**63 - 1)
    shown = run(home, "--json", "sweep", "--status")
    assert shown.returncode == 0 and json.loads(shown.stdout)["adopted"] == 2**63 - 1, shown
    assert f"adopted\t{2**63 - 1}\n" in run(home, "sweep", "--status").stdout
    for bad in (2**63, 1.5, -1):
        write_receipt(home, adopted=bad)
        refused = run(home, "sweep", "--status")
        assert refused.returncode == 1 and "is not a receipt" in refused.stderr, (bad, refused)


def check_r12_sweep_status_prints_the_receipt_shape_only(data, workspace):
    """`sweep --json --status` prints `at`, `seconds`, `trees` and the counts
    and nothing else the file holds, and a tree that is not an absolute path in
    its clean form makes the file no receipt."""
    home, _ = fresh(workspace, "r12-receipt-shape")
    write_receipt(home, extra=1)
    shown = run(home, "--json", "sweep", "--status")
    assert shown.returncode == 0, shown
    assert json.loads(shown.stdout) == {
        "at": 1700000000, "seconds": 1.5, "trees": ["/a"], "adopted": 0, "merged": 0, "gone": 0,
        "unreadable": 0, "skipped": 0, "listed": 0, "unlistable": 0}, shown.stdout
    for tree in ("a/b", "/a/../b", "/a/", "", 3):
        write_receipt(home, trees=[tree])
        for words in (("sweep", "--status"), ("--json", "sweep", "--status")):
            refused = run(home, *words)
            assert refused.returncode == 1 and refused.stdout == "", (tree, words, refused)
            assert "is not a receipt" in refused.stderr, (tree, refused)


def check_r13_trash_drops_the_rows_gio_reports_gone(data, workspace):
    """A row drops where `gio trash` exits 0; one it refuses keeps its row and
    its reason, and the operands after it are still tried."""
    home, w = fresh(workspace, "r13-trash-gio-says")
    wrap = fake_gio(home, 'case "$2" in */stuck) echo "$2: no trash here" >&2; exit 1;; esac')
    if not wrap:
        return
    for name in ("a", "stuck", "b"):
        (w / name).write_text(name)
    assert run(home, "tag", "add", "--tag", "t", "--", w / "a", w / "stuck", w / "b").returncode == 0
    done = run(home, "-y", "trash", w / "a", w / "stuck", w / "b", wrap=wrap)
    assert done.returncode == 1, (done.stdout, done.stderr)
    assert done.stderr == f"trash: {w}/stuck: no trash here\n", done.stderr
    assert not (w / "a").exists() and (w / "stuck").exists() and not (w / "b").exists()
    assert [json.loads(row)["path"] for row in run(home, "export").stdout.splitlines()] == \
        [str(w / "stuck")]


def check_desktop_file_manager(data, workspace):
    """Desktop registration, isolated MIME defaults and the running instance."""
    import configparser
    desktop = configparser.ConfigParser(interpolation=None)
    desktop.read(ROOT / "pkgbuild/omafiles.desktop")
    assert desktop["Desktop Entry"]["Exec"] == "omafiles-studio %U"
    assert desktop["Desktop Entry"]["MimeType"] == "inode/directory;"
    service = configparser.ConfigParser()
    service.read(ROOT / "pkgbuild/org.freedesktop.FileManager1.service")
    assert service["D-BUS Service"]["Name"] == "org.freedesktop.FileManager1"
    assert service["D-BUS Service"]["Exec"] == "/usr/bin/omafiles-studio"
    home, w = fresh(workspace, "desktop-manager")
    env = {**cli_env(home), "QT_QPA_PLATFORM": "offscreen", "QT_QPA_PLATFORMTHEME": ""}
    for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    temporary = home / "tmp"
    temporary.mkdir()
    env["TMPDIR"] = str(temporary)
    runtime = home / "runtime"
    runtime.mkdir(mode=0o700)
    env["XDG_RUNTIME_DIR"] = str(runtime)
    applications = Path(env["XDG_DATA_HOME"]) / "applications"
    applications.mkdir()
    shutil.copy(ROOT / "pkgbuild/omafiles.desktop", applications)
    chosen = subprocess.run(["xdg-mime", "default", "omafiles.desktop", "inode/directory"],
                            env=env, capture_output=True, timeout=10)
    assert chosen.returncode == 0, chosen.stderr
    answer = subprocess.run(["xdg-mime", "query", "default", "inode/directory"],
                            env=env, capture_output=True, timeout=10)
    assert answer.stdout.strip() == b"omafiles.desktop", answer
    studio = CLI.parent / "omafiles-studio"
    if not studio.is_file():
        return
    with subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as bus:
        try:
            env["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().decode().strip()
            assert env["DBUS_SESSION_BUS_ADDRESS"], bus.stderr.read().decode()
            folder = w / "folder # percent %"
            folder.mkdir()
            file = w / "file.txt"
            file.write_text("file")
            for words in ([str(w / "missing")], [folder.as_uri(), "file:///tmp/%ff"]):
                refused = subprocess.run([studio, *words], env=env, capture_output=True, timeout=5)
                assert refused.returncode == 2, refused
            with subprocess.Popen([studio, folder.as_uri(), str(file)], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as primary:
                try:
                    query = ["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                             "--object-path", "/org/freedesktop/DBus", "--method",
                             "org.freedesktop.DBus.NameHasOwner", "org.omafiles.Studio"]
                    deadline = time.monotonic() + 10
                    while True:
                        owned = subprocess.run(query, env=env, capture_output=True, timeout=5)
                        if b"true" in owned.stdout:
                            break
                        assert primary.poll() is None, primary.stderr.read().decode()
                        assert time.monotonic() < deadline, "instance never registered"
                        time.sleep(0.02)
                    for words in ([], [str(folder), file.as_uri()], ["folder # percent %"]):
                        secondary = subprocess.run([studio, *words], cwd=w, env=env,
                                                   capture_output=True, timeout=5)
                        assert secondary.returncode == 0, secondary.stderr
                        assert primary.poll() is None, primary.stderr.read().decode()
                    with subprocess.Popen(["dbus-monitor", "--session",
                            "type='method_call',interface='org.freedesktop.FileManager1',member='Open'"],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as monitor:
                        try:
                            assert select.select([monitor.stdout], [], [], 5)[0], "monitor never became ready"
                            token_env = {**env, "XDG_ACTIVATION_TOKEN": "wayland-test-token",
                                         "DESKTOP_STARTUP_ID": "startup-test-id"}
                            for token in ("wayland-test-token", ""):
                                token_env["XDG_ACTIVATION_TOKEN"] = token
                                done = subprocess.run([studio], env=token_env, capture_output=True, timeout=5)
                                assert done.returncode == 0, done.stderr
                        finally:
                            monitor.terminate()
                            traffic, diagnostic = monitor.communicate(timeout=5)
                        assert b'string "wayland-test-token"' in traffic, (traffic, diagnostic)
                        assert b'string "startup-test-id"' in traffic, (traffic, diagnostic)
                    for method in ("ShowFolders", "ShowItems", "ShowItemProperties"):
                        reply = subprocess.run(["gdbus", "call", "--session", "--dest",
                            "org.freedesktop.FileManager1", "--object-path",
                            "/org/freedesktop/FileManager1", "--method",
                            "org.freedesktop.FileManager1." + method,
                            "['" + folder.as_uri() + "']", ""], env=env,
                            capture_output=True, timeout=5)
                        assert reply.returncode == 0, (method, reply.stderr)
                finally:
                    primary.terminate()
                    primary.wait(timeout=5)
        finally:
            bus.terminate()
            bus.wait(timeout=5)


def check_tag_rename_on_selected_paths(data, workspace):
    """A scoped rename edits only selected carriers, merges valid xattr tags,
    and refuses a bad set without changing its index or mirrors."""
    home, w = fresh(workspace, "window-tag-rename")
    a, b, outside, plain = [w / name for name in ("-a", "b", "outside", "plain")]
    for path in (a, b, outside, plain):
        path.write_text("body")
    seeded = run(home, "tag", "add", "--tag", "old", "--", a, b, outside)
    assert seeded.returncode == 0, seeded
    assert run(home, "tag", "add", "--tag", "old-keep", "--", a).returncode == 0
    if carries_xattrs(a):
        os.setxattr(a, "user.omafiles.tags", b"old,local,old-keep")
        os.setxattr(plain, "user.omafiles.tags", b"untouched")
    renamed = run(home, "--json", "tag", "rename", "OLD", "New", "--", a, b, plain)
    assert renamed.returncode == 0, renamed
    receipt = json.loads(renamed.stdout)
    assert {row["path"] for row in receipt} == {str(a), str(b)}, receipt
    for path in (a, b):
        row = json.loads(run(home, "json", "--", path).stdout)
        assert "new" in row["tags"] and "old" not in row["tags"], row
    assert run(home, "json", "--", plain).stdout == ""
    if carries_xattrs(plain):
        assert os.getxattr(plain, "user.omafiles.tags") == b"untouched"
    assert json.loads(run(home, "json", "--", outside).stdout)["tags"] == ["old"]
    if carries_xattrs(a):
        row = json.loads(run(home, "json", "--", a).stdout)
        assert row["tags"] == ["local", "new", "old-keep"], row
        assert os.getxattr(a, "user.omafiles.tags") == b"local,new,old-keep"
    before = (home / "omafiles/index.jsonl").read_bytes()
    refused = run(home, "tag", "rename", "new", "again", "--", a, w / "missing")
    assert refused.returncode == 2 and refused.stdout == "", refused
    assert (home / "omafiles/index.jsonl").read_bytes() == before
    invalid = run(home, "tag", "rename", "new", "not valid", "--", a)
    assert invalid.returncode == 1 and "invalid tag" in invalid.stderr, invalid
    assert (home / "omafiles/index.jsonl").read_bytes() == before
    same = run(home, "tag", "rename", "new", "NEW", "--", a)
    assert same.returncode == 1 and "same tag" in same.stderr, same
    assert (home / "omafiles/index.jsonl").read_bytes() == before
    preview = run(home, "--dry-run", "tag", "rename", "new", "preview", "--", a)
    assert preview.returncode == 0 and str(a) in preview.stdout, preview
    assert (home / "omafiles/index.jsonl").read_bytes() == before
    complete = run(home, "__complete", "tag", "rename", "")
    assert complete.returncode == 0 and "new" in complete.stdout.splitlines(), complete
    assert run(home, "__complete", "tag", "rename", "old", "new", "").returncode == 1
    batch = run(home, "batch", "-", stdin=f"tag rename new batch -- '{a}'\n")
    assert batch.returncode == 0, batch
    assert json.loads(run(home, "json", "--", a).stdout)["tags"][-1] != "new"
    merged = run(home, "tag", "rename", "batch", "old-keep", "--", a)
    assert merged.returncode == 0, merged
    tags = json.loads(run(home, "json", "--", a).stdout)["tags"]
    assert "batch" not in tags and tags.count("old-keep") == 1, tags
    if carries_xattrs(a):
        assert os.getxattr(a, "user.omafiles.tags") == ",".join(tags).encode()
    global_rename = run(home, "tag", "rename", "old", "global")
    assert global_rename.returncode == 0, global_rename
    assert json.loads(run(home, "json", "--", outside).stdout)["tags"] == ["global"]


def check_rename_refuses_a_late_collision(data, workspace):
    """The kernel refuses a destination created after userspace preflight."""
    home, room = fresh(workspace, "rename-late-collision")
    shim = room / "rename-race.so"
    source_code = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static int move(int olddir, const char *old, int newdir, const char *to, unsigned flags) {
    const char *expected = getenv("OMAFILES_TEST_RACE_SOURCE");
    if (expected && strcmp(expected, old) == 0) {
        int fd = openat(newdir, to, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { write(fd, "occupied", 8); close(fd); }
    }
    return syscall(SYS_renameat2, olddir, old, newdir, to, flags);
}
int rename(const char *old, const char *to) { return move(AT_FDCWD, old, AT_FDCWD, to, 0); }
int renameat2(int olddir, const char *old, int newdir, const char *to, unsigned flags) {
    return move(olddir, old, newdir, to, flags);
}
"""
    built = subprocess.run(["cc", "-shared", "-fPIC", "-x", "c", "-", "-o", str(shim)],
                           input=source_code, text=True, capture_output=True, check=False)
    assert built.returncode == 0, built.stderr
    for pattern in (False, True):
        source = room / ("pattern" if pattern else "single")
        destination = room / (source.name + "-new")
        source.write_text("original")
        assert run(home, "tag", "add", "--tag", "safe", "--", source).returncode == 0
        words = ["mv", "--pattern", "{name}-new", "--", source] if pattern else ["mv", "--", source, destination]
        result = run(home, *words, extra_env={"LD_PRELOAD": str(shim),
                     "OMAFILES_TEST_RACE_SOURCE": str(source)})
        assert result.returncode != 0 and "File exists" in result.stderr, result
        assert source.read_text() == "original"
        assert destination.read_text() == "occupied"
        assert json.loads(run(home, "json", "--", source).stdout)["tags"] == ["safe"]


def check_transfer_conflict_policies(data, workspace):
    """Conflict decisions preserve the winner's bytes and tags without following a destination link."""
    room = workspace / "transfer-conflicts"
    room.mkdir()
    for verb in ("cp", "mv"):
        for policy in ("-f", "-n", "--keep-both", "-b"):
            case = room / (verb + policy)
            case.mkdir()
            source, target = case / "incoming.txt", case / "existing.txt"
            source.write_text("incoming")
            target.write_text("existing")
            for path, tag in ((source, "incoming"), (target, "existing")):
                tagged = run(data, "tag", "add", "--tag", tag, "--", path)
                assert tagged.returncode == 0, tagged.stderr
            if policy == "-b":
                preview = run(data, "--json", "--dry-run", verb, policy, "-T", "--", source, target)
                assert preview.returncode == 0, preview.stderr
                planned = json.loads(preview.stdout)
                assert len(planned) == 2 and planned[0]["to"].endswith("existing.txt.~1~")
                assert target.read_text() == "existing" and source.read_text() == "incoming"
            result = run(data, "--json", verb, policy, "-T", "--", source, target)
            assert result.returncode == 0, result.stderr
            if policy == "-n":
                assert not result.stdout and source.read_text() == "incoming"
                assert target.read_text() == "existing"
                winner = target
                expected = "existing"
            else:
                receipt = json.loads(result.stdout)
                if policy == "-b":
                    assert len(receipt) == 2
                    backup = case / "existing.txt.~1~"
                    assert backup.read_text() == "existing"
                    assert json.loads(run(data, "json", backup).stdout)["tags"] == ["existing"]
                    receipt = receipt[-1]
                winner = Path(receipt["to"])
                expected = "incoming"
                assert winner.read_text() == "incoming"
                assert source.exists() == (verb == "cp")
                if policy == "--keep-both":
                    assert winner.name == "existing (1).txt"
                    assert target.read_text() == "existing"
                assert not list(case.glob(".omafiles-replaced-*"))
            assert json.loads(run(data, "json", winner).stdout)["tags"] == [expected]
    source = room / "source"
    source.write_text("source")
    sentinel = room / "sentinel"
    sentinel.write_text("sentinel")
    link = room / "link"
    link.symlink_to(sentinel)
    replaced = run(data, "cp", "-f", "-T", "--", source, link)
    assert replaced.returncode == 0, replaced.stderr
    assert not link.is_symlink() and link.read_text() == "source"
    assert sentinel.read_text() == "sentinel"
    link.unlink()
    link.symlink_to(room / "missing")
    skipped = run(data, "--json", "cp", "-n", "-T", "--", source, link)
    assert skipped.returncode == 0 and not skipped.stdout and link.is_symlink()
    kept = run(data, "cp", "--keep-both", "-T", "--", source, link)
    assert kept.returncode == 0, kept.stderr
    assert (room / "link (1)").read_text() == "source" and link.is_symlink()
    interactive = run(data, "cp", "-i", "-T", "--", source, link)
    assert interactive.returncode == 1 and "terminal" in interactive.stderr
    code, text = run_pty(data, "cp", "-i", "-T", "--", source, link, answer="n\n")
    assert code == 0 and link.is_symlink(), text
    code, text = run_pty(data, "cp", "-i", "-T", "--", source, link, answer="y\n")
    assert code == 0 and not link.is_symlink(), text
    same = run(data, "cp", "-f", "-T", "--", source, source)
    assert same.returncode == 1 and source.read_text() == "source"
    target = room / "dry-target"
    target.write_text("old")
    dry = run(data, "--dry-run", "cp", "-f", "-T", "--", source, target)
    assert dry.returncode == 0 and target.read_text() == "old" and source.exists()
    folders = room / "folder"
    folders.mkdir()
    (folders / "child").write_text("protected")
    other = room / "other-folder"
    other.mkdir()
    refusal = run(data, "mv", "-f", "-T", "--", other, folders)
    assert refusal.returncode == 1 and "nonempty" in refusal.stderr
    assert other.is_dir() and (folders / "child").read_text() == "protected"
    (folders / "child").unlink()
    allowed = run(data, "mv", "-f", "-T", "--", other, folders)
    assert allowed.returncode == 0 and not other.exists() and folders.is_dir(), allowed.stderr
    assert not list(room.rglob(".omafiles-replaced-*"))


if __name__ == "__main__":
    main()
