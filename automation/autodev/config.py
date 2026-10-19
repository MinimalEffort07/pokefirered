"""Settings the dispatcher needs: where its board, repo and sessions are."""

import json
import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Config:
    owner: str          # GitHub user that owns the board (the human, not the bot)
    repo: str           # "owner/name" of the repo whose issues autodev works on
    board_title: str    # title of the user-owned Projects (v2) board
    bot: str = ""       # autodev's own GitHub account
    max_sessions: int = 2
    model: str = "opus"
    claude: str = ""            # path to the claude CLI
    claude_config_dir: str = "" # the owner's Claude login and plugins
    env_file: str = ""          # the bot's GH_TOKEN
    clone: str = ""             # autodev's own clone
    log_dir: str = ""
    state_dir: str = ""
    resume_command: str = "claude --resume {session_id}"


def load(path):
    with open(path) as f:
        values = json.load(f)
    # Paths may start with ~ so the file reads the same on any account.
    for key, value in values.items():
        if isinstance(value, str) and value.startswith("~/"):
            values[key] = os.path.expanduser(value)
    return Config(**values)
