#!/usr/bin/env python3

import os
import sys
import tempfile
import shutil
import subprocess
import unittest
from pathlib import Path

# A git hook exports GIT_DIR and GIT_INDEX_FILE; the scratch repositories must not inherit them.
for name in [name for name in os.environ if name.startswith("GIT_")]:
    del os.environ[name]


def run_link_check(repo_dir):
    result = subprocess.run(
        [sys.executable, os.path.join(os.path.dirname(__file__), "../scripts/link-check.py"), repo_dir],
        capture_output=True,
        text=True,
    )
    return result.returncode, result.stdout.strip()


class TestLinkCheck(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp()
        self.repo_dir = os.path.join(self.temp_dir, "test_repo")
        os.makedirs(self.repo_dir)
        subprocess.run(["git", "init"], cwd=self.repo_dir, capture_output=True)
        subprocess.run(
            ["git", "config", "user.email", "test@example.com"],
            cwd=self.repo_dir,
            capture_output=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Test User"],
            cwd=self.repo_dir,
            capture_output=True,
        )

    def tearDown(self):
        shutil.rmtree(self.temp_dir)

    def create_md_file(self, rel_path, content):
        full_path = os.path.join(self.repo_dir, rel_path)
        os.makedirs(os.path.dirname(full_path), exist_ok=True)
        with open(full_path, "w") as f:
            f.write(content)
        subprocess.run(["git", "add", rel_path], cwd=self.repo_dir, capture_output=True)

    def test_missing_file(self):
        self.create_md_file(
            "test.md",
            "[link to missing](missing.md)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 1)
        self.assertIn("test.md:1:", output)
        self.assertIn("link target not found", output)

    def test_missing_image(self):
        self.create_md_file(
            "test.md",
            "![alt text](missing.png)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 1)
        self.assertIn("test.md:1:", output)
        self.assertIn("image target not found", output)

    def test_bad_anchor_same_file(self):
        self.create_md_file(
            "test.md",
            """# Heading One

[link to bad anchor](#nonexistent)
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 1)
        self.assertIn("test.md:3:", output)
        self.assertIn("anchor not found", output)

    def test_good_anchor_same_file(self):
        self.create_md_file(
            "test.md",
            """# Heading One

[link to heading](#heading-one)
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_good_anchor_other_file(self):
        self.create_md_file(
            "test1.md",
            """# My Heading

Content here.
"""
        )
        self.create_md_file(
            "test2.md",
            "[link to other](test1.md#my-heading)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_duplicate_heading_first_occurrence(self):
        self.create_md_file(
            "test.md",
            """# Note

First note.

# Note

Second note.

[link to first](#note)
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_duplicate_heading_second_occurrence(self):
        self.create_md_file(
            "test.md",
            """# Note

First note.

# Note

Second note.

[link to second](#note-1)
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_duplicate_heading_bad_suffix(self):
        self.create_md_file(
            "test.md",
            """# Note

First note.

# Note

Second note.

[link to bad](#note-2)
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 1)
        self.assertIn("test.md:9:", output)

    def test_link_in_code_fence_ignored(self):
        self.create_md_file(
            "test.md",
            """# Heading

```
[link in code](missing.md)
```
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_link_in_inline_code_ignored(self):
        self.create_md_file(
            "test.md",
            """# Heading

See `[link in code](missing.md)` for details.
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_absolute_url_ignored(self):
        self.create_md_file(
            "test.md",
            "[link](https://example.com)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_mailto_ignored(self):
        self.create_md_file(
            "test.md",
            "[email](mailto:user@example.com)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_reference_style_link_missing_file(self):
        self.create_md_file(
            "test.md",
            """[link text][ref]

[ref]: missing.md
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 1)
        self.assertIn("test.md:1:", output)

    def test_reference_style_link_good(self):
        self.create_md_file(
            "test.md",
            "Test content."
        )
        self.create_md_file(
            "test2.md",
            """[link text][ref]

[ref]: test.md
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_heading_with_special_chars(self):
        self.create_md_file(
            "test.md",
            """# Hello, World!

[link](#hello-world)
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_multiple_links_on_same_line(self):
        self.create_md_file(
            "test.md",
            "[good](test.md) and [bad](missing.md)"
        )
        self.create_md_file(
            "other.md",
            "Content"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 1)
        self.assertIn("test.md:1:", output)

    def test_image_with_title(self):
        self.create_md_file(
            "test.md",
            "Content"
        )
        self.create_md_file(
            "test2.md",
            '![alt](test.md "title")'
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_subdirectory_link(self):
        self.create_md_file(
            "docs/test.md",
            "Content here"
        )
        self.create_md_file(
            "test.md",
            "[link to doc](docs/test.md)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_relative_up_link(self):
        self.create_md_file(
            "docs/sub/test.md",
            "Content here"
        )
        self.create_md_file(
            "docs/readme.md",
            "[link](sub/test.md)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_same_file_anchor_only(self):
        self.create_md_file(
            "test.md",
            """# Section

[jump to section](#section)

More content.
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_heading_with_inline_code(self):
        self.create_md_file(
            "test.md",
            """## `omafiles ls`

[link](#omafiles-ls)
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_heading_with_link(self):
        self.create_md_file(
            "guide.md",
            "Content."
        )
        self.create_md_file(
            "test.md",
            """## See [the guide](guide.md) for details

[link](#see-the-guide-for-details)
"""
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_angle_bracket_target(self):
        self.create_md_file(
            "my file.md",
            "Content."
        )
        self.create_md_file(
            "test.md",
            "[link](<my file.md>)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_url_encoded_path(self):
        self.create_md_file(
            "my file.md",
            "Content."
        )
        self.create_md_file(
            "test.md",
            "[link](my%20file.md)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 0)
        self.assertEqual(output, "")

    def test_multiple_files_with_errors(self):
        self.create_md_file(
            "test1.md",
            "[bad](missing.md)"
        )
        self.create_md_file(
            "test2.md",
            "[bad anchor](#nonexistent)"
        )
        returncode, output = run_link_check(self.repo_dir)
        self.assertEqual(returncode, 1)
        lines = output.split("\n")
        self.assertGreaterEqual(len(lines), 2)


if __name__ == "__main__":
    unittest.main()
