"""Settings the dispatcher needs to find its board and repo."""

import json
from dataclasses import dataclass


@dataclass(frozen=True)
class Config:
    owner: str          # GitHub user that owns the board (the human, not the bot)
    repo: str           # "owner/name" of the repo whose issues autodev works on
    board_title: str    # title of the user-owned Projects (v2) board


def load(path):
    with open(path) as f:
        return Config(**json.load(f))
