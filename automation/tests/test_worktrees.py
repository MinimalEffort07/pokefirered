"""Each ticket's worktree in autodev's clone (CLAUDE.md step 6).

These run real git against a throwaway "origin" and clone in a temporary
folder, because what matters is what git does: which commit the branch
starts from, whether it tracks anything, and what a second call does.
"""

import os
import subprocess
import tempfile
import unittest

from autodev import worktrees


def git(*args, cwd):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                           *args], cwd=cwd, check=True, capture_output=True,
                          text=True).stdout.strip()


class Worktree(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = tmp.name
        # The real agbcc lives in the owner's checkout; the clone's
        # tools/agbcc is a symlink to it (setup.sh).
        self.agbcc = os.path.join(root, "main", "tools", "agbcc")
        os.makedirs(self.agbcc)
        seed = os.path.join(root, "seed")
        os.makedirs(os.path.join(seed, "tools"))
        open(os.path.join(seed, "tools", "README"), "w").close()
        git("init", "-q", "-b", "master", cwd=seed)
        git("add", ".", cwd=seed)
        git("commit", "-q", "-m", "first", cwd=seed)
        origin = os.path.join(root, "origin.git")
        git("clone", "-q", "--bare", seed, origin, cwd=root)
        self.clone = os.path.join(root, "clone")
        git("clone", "-q", origin, self.clone, cwd=root)
        os.symlink(self.agbcc, os.path.join(self.clone, "tools", "agbcc"))
        self.master = git("rev-parse", "origin/master", cwd=self.clone)

    def test_a_ticket_gets_its_branch_from_master_with_agbcc_linked(self):
        path = worktrees.make(self.clone, 68, "issue-68-note")
        self.assertEqual(path, os.path.join(self.clone, ".worktrees", "issue-68"))
        self.assertEqual(git("branch", "--show-current", cwd=path), "issue-68-note")
        self.assertEqual(git("rev-parse", "HEAD", cwd=path), self.master)
        self.assertEqual(os.readlink(os.path.join(path, "tools", "agbcc")),
                         self.agbcc)

    def test_the_branch_tracks_nothing_so_a_bare_push_cannot_reach_master(self):
        # A branch cut from origin/master would track it by default, and a
        # plain `git push` could then try to update master.
        path = worktrees.make(self.clone, 68, "issue-68-note")
        upstream = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "@{upstream}"], cwd=path,
            capture_output=True, text=True)
        self.assertNotEqual(upstream.returncode, 0)

    def test_it_starts_from_origin_master_whatever_the_clone_has_checked_out(self):
        # When a branch of autodev itself is live-tested, the clone sits on
        # that branch; tickets must still start from master.
        git("checkout", "-q", "-b", "autodev-under-test", cwd=self.clone)
        git("commit", "-q", "--allow-empty", "-m", "test", cwd=self.clone)
        path = worktrees.make(self.clone, 68, "issue-68-note")
        self.assertEqual(git("rev-parse", "HEAD", cwd=path), self.master)

    def test_a_second_call_leaves_the_existing_worktree_alone(self):
        # The launch after it failed, so the next tick asks again.
        path = worktrees.make(self.clone, 68, "issue-68-note")
        open(os.path.join(path, "work-in-progress"), "w").close()
        self.assertEqual(worktrees.make(self.clone, 68, "issue-68-note"), path)
        self.assertTrue(os.path.exists(os.path.join(path, "work-in-progress")))


class BranchNames(unittest.TestCase):
    def test_the_number_and_the_first_five_words_of_the_title(self):
        for title, expected in [
            ("Note the grill path's live check at the end of automation/README.md",
             "issue-68-note-the-grill-paths-live"),
            ("PC Anywhere", "issue-68-pc-anywhere"),
            ("!!!", "issue-68"),
        ]:
            with self.subTest(title=title):
                self.assertEqual(worktrees.branch_name(68, title), expected)


if __name__ == "__main__":
    unittest.main()
