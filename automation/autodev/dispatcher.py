"""The autodev dispatcher: one run per timer tick.

For now a tick only reads the board and logs a snapshot of it; it takes
no action on GitHub. systemd sends what it prints to the journal
(`journalctl --user -u autodev-dispatcher`).
"""

from autodev import board


def tick(gh, config, log):
    """Log one snapshot of the board. Returns the process exit code."""
    number = board.find_board(gh, config)
    if number is None:
        print('error: no board titled "%s" owned by %s' % (
            config.board_title, config.owner), file=log)
        return 1
    snapshot = board.read_board(gh, config, number)
    header = 'board #%d "%s": %s' % (
        snapshot.number, snapshot.title, _count(len(snapshot.items), "item"))
    if snapshot.skipped:
        header += ", %s skipped" % _count(snapshot.skipped, "other card")
    print(header, file=log)
    for item in snapshot.items:
        print("#%d %s %s has_parent=%s" % (
            item.number, item.author, item.status,
            "yes" if item.has_parent else "no"), file=log)
    return 0


def _count(n, noun):
    return "%d %s%s" % (n, noun, "" if n == 1 else "s")
