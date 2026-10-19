"""Each ticket's own git worktree in autodev's clone.

A worktree is a second working folder of the same repository, checked out
on its own branch. Every ticket gets one at .worktrees/issue-<T> on branch
issue-<T>-<slug> (CLAUDE.md step 6), so ticket sessions running at the
same time never share a working tree. Its fix-up and merge sessions reuse
it later.
"""

import os
import re
import subprocess


def branch_name(number, title):
    """issue-<number>-<first words of the title>, e.g. "issue-68-note-the-
    grill-paths-live". Apostrophes are dropped so "path's" stays one word."""
    words = re.split(r"[^a-z0-9]+", title.lower().replace("'", ""))
    slug = "-".join(w for w in words if w)
    slug = "-".join(slug.split("-")[:5])[:40].rstrip("-")
    return "issue-%d-%s" % (number, slug) if slug else "issue-%d" % number


def path(clone, number):
    return os.path.join(clone, ".worktrees", "issue-%d" % number)


def make(clone, number, branch):
    """Create ticket `number`'s worktree on a new `branch` cut from master,
    unless it already exists. Returns the worktree's path."""
    where = path(clone, number)
    if os.path.isdir(where):
        return where    # an earlier tick made it (and maybe its launch failed)

    def git(*args):
        subprocess.run(["git", "-C", clone, *args], check=True,
                       capture_output=True, text=True, timeout=120)

    # origin/master, not whatever the clone has checked out: while a branch
    # of autodev itself is being tried out live, the clone sits on that.
    git("fetch", "--quiet", "origin", "master")
    # --no-track: a branch cut from origin/master would otherwise track it,
    # and a plain `git push` from the session could then aim at master.
    git("worktree", "add", "--quiet", "--no-track", "-b", branch, where,
        "origin/master")
    # agbcc, the game's compiler, isn't in git (CLAUDE.md, step 6): link the
    # one the clone uses, which is itself a link to the owner's checkout.
    os.symlink(os.path.realpath(os.path.join(clone, "tools", "agbcc")),
               os.path.join(where, "tools", "agbcc"))
    return where
