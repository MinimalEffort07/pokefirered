"""The real GitHub adapter: runs `gh api` as a subprocess.

`gh` reads its token from the GH_TOKEN environment variable when it is
set. The systemd units set it to the bot's token (from
~/.config/autodev/env), so everything the dispatcher does on GitHub is
done as the bot, never as the owner.
"""

import json
import subprocess


class GhError(RuntimeError):
    pass


# A tick runs every 2 minutes; one gh call that hangs (a stalled network)
# must not hold the dispatcher forever, so each call gets a time limit.
TIMEOUT_SECONDS = 60


def _fields(values):
    args = []
    for name, value in values.items():
        if value is None:
            continue  # an unset GraphQL variable means "first page"
        # -F sends numbers as numbers; -f always sends a string.
        flag = "-F" if isinstance(value, int) else "-f"
        args += [flag, "%s=%s" % (name, value)]
    return args


def _run(args):
    try:
        result = subprocess.run(["gh"] + args, capture_output=True, text=True,
                                timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        raise GhError("gh took longer than %d s" % TIMEOUT_SECONDS)
    if result.returncode != 0:
        raise GhError(result.stderr.strip() or "gh exited %d" % result.returncode)
    return result.stdout


class GhCli:
    def graphql(self, query, **variables):
        return json.loads(_run(["api", "graphql", "-f", "query=" + query]
                               + _fields(variables)))

    def rest(self, method, path, **fields):
        """One REST call, e.g. rest("POST", "repos/o/r/issues/1/comments",
        body="hi"). Returns the decoded JSON, or None for an empty reply."""
        out = _run(["api", "-X", method, path] + _fields(fields))
        return json.loads(out) if out.strip() else None

    def rest_list(self, path):
        """Every element of a paged REST list (e.g. an issue's comments).
        `--paginate` follows the pages; `--jq '.[]'` prints one element per
        line, so the pages don't have to be stitched together here."""
        out = _run(["api", "--paginate", path, "--jq", ".[]"])
        return [json.loads(line) for line in out.splitlines() if line.strip()]
