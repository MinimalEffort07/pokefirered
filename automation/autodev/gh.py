"""The real GitHub adapter: runs `gh api graphql` as a subprocess.

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


class GhCli:
    def graphql(self, query, **variables):
        args = ["gh", "api", "graphql", "-f", "query=" + query]
        for name, value in variables.items():
            if value is None:
                continue  # an unset GraphQL variable means "first page"
            # -F sends numbers as numbers; -f always sends a string.
            flag = "-F" if isinstance(value, int) else "-f"
            args += [flag, "%s=%s" % (name, value)]
        try:
            result = subprocess.run(args, capture_output=True, text=True,
                                    timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            raise GhError("gh took longer than %d s" % TIMEOUT_SECONDS)
        if result.returncode != 0:
            raise GhError(result.stderr.strip() or "gh exited %d" % result.returncode)
        return json.loads(result.stdout)
