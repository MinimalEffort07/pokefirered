"""Entry point the systemd timer runs: `python3 -m autodev tick`.

Run it from the automation/ directory (the unit's WorkingDirectory).
"""

import os
import sys

from autodev import config as config_module
from autodev.dispatcher import tick
from autodev.gh import GhCli, GhError

CONFIG_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                           "config.json")


def main(argv):
    if argv[1:] != ["tick"]:
        print("usage: python3 -m autodev tick", file=sys.stderr)
        return 2
    try:
        return tick(GhCli(), config_module.load(CONFIG_PATH), sys.stdout)
    except GhError as e:
        print("error: gh failed: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
