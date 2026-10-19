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


class GhCli:
    def graphql(self, query, **variables):
        args = ["gh", "api", "graphql", "-f", "query=" + query]
        for name, value in variables.items():
            if value is None:
                continue  # an unset GraphQL variable means "first page"
            # -F sends numbers as numbers; -f always sends a string.
            flag = "-F" if isinstance(value, int) else "-f"
            args += [flag, "%s=%s" % (name, value)]
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode != 0:
            raise GhError(result.stderr.strip() or "gh exited %d" % result.returncode)
        return json.loads(result.stdout)
