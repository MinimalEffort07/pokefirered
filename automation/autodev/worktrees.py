"""Each ticket's own git worktree in autodev's clone.

A worktree is a second working folder of the same repository, checked out
on its own branch. Every ticket gets one at <clone>-worktrees/issue-<T>
on branch issue-<T>-<slug> (CLAUDE.md step 6), so ticket sessions running
at the same time never share a working tree. Its fix-up and merge
sessions reuse it later.

The worktrees sit beside the clone, not inside it. Claude reads every
CLAUDE.md in the folders above its working folder, so a session in a
worktree inside the clone would load the clone's CLAUDE.md as well as the
worktree's own, and the two differ whenever the ticket edits CLAUDE.md or
master moves on.
"""

import os
import re
import subprocess


def branch_prefix(number):
    """What every branch of ticket `number` starts with. open-pr and the
    dispatcher's PR check (github.ticket_outcome) both rely on it."""
    return "issue-%d-" % number


def branch_name(number, title):
    """issue-<number>-<slug>: the slug is the title's first five words,
    stopping early rather than going over 40 characters, e.g. "issue-68-
    note-the-grill-paths-live". Apostrophes are dropped so "path's" stays
    one word; a title with no letters or digits gives "ticket"."""
    words = [w for w in re.split(r"[^a-z0-9]+", title.lower().replace("'", "")) if w]
    slug = ""
    for word in words[:5]:
        longer = slug + "-" + word if slug else word
        if len(longer) > 40:
            break
        slug = longer
    if not slug:
        slug = words[0][:40] if words else "ticket"
    return branch_prefix(number) + slug


def path(clone, number):
    """e.g. ~/projects/minimaleffort/pokefirered-autodev-worktrees/issue-68"""
    return os.path.join(clone + "-worktrees", "issue-%d" % number)


def tickets_with_worktrees(clone):
    """The numbers of the tickets that have a worktree."""
    try:
        names = os.listdir(clone + "-worktrees")
    except FileNotFoundError:
        return set()
    return {int(n[len("issue-"):]) for n in names
            if re.fullmatch(r"issue-\d+", n)}


def _git(clone, *args, check=True):
    return subprocess.run(["git", "-C", clone, *args], check=check,
                          capture_output=True, text=True, timeout=120)


def make(clone, number, branch):
    """Make sure ticket `number` has its worktree on `branch`, with agbcc
    linked, and return its path. Safe to repeat: an earlier tick may have
    made part of it before failing."""
    where = path(clone, number)

    def git(*args):
        return _git(clone, *args)

    def has(ref):
        return _git(clone, "rev-parse", "--verify", "--quiet", ref,
                    check=False).returncode == 0

    if not os.path.isdir(where):
        # git still lists a worktree whose folder was deleted, and won't
        # check its branch out again until it forgets that one.
        git("worktree", "prune")
        if has("refs/heads/" + branch):
            # The folder went (by hand, or when the ticket closed) but the
            # branch, with any earlier work on it, is still here.
            git("worktree", "add", "--quiet", where, branch)
        else:
            git("fetch", "--quiet", "origin")
            if has("refs/remotes/origin/" + branch):
                # Only GitHub has it (a merge session for a ticket whose
                # worktree is gone): check out the ticket's work from there.
                git("worktree", "add", "--quiet", "--track", "-b", branch,
                    where, "origin/" + branch)
            else:
                # A new ticket starts from origin/master, not whatever the
                # clone has checked out: while a branch of autodev itself
                # is tried out live, the clone sits on that. --no-track: a
                # branch cut from origin/master would otherwise track it,
                # and a plain `git push` could then aim at master.
                git("worktree", "add", "--quiet", "--no-track", "-b", branch,
                    where, "origin/master")
    # agbcc, the game's compiler, isn't in git (CLAUDE.md, step 6): link the
    # one the clone uses, which is itself a link to the owner's checkout.
    link = os.path.join(where, "tools", "agbcc")
    if not os.path.lexists(link):
        os.symlink(os.path.realpath(os.path.join(clone, "tools", "agbcc")), link)
    return where


def remove(clone, number):
    """Delete ticket `number`'s worktree and its local branch, once the
    ticket is closed. The branch on GitHub stays. Safe to repeat."""
    where = path(clone, number)
    if os.path.isdir(where):
        branch = _git(where, "branch", "--show-current").stdout.strip()
        # --force: whatever is left in it is no longer wanted.
        _git(clone, "worktree", "remove", "--force", where)
        if branch:
            _git(clone, "branch", "-D", branch)
    _git(clone, "worktree", "prune")
