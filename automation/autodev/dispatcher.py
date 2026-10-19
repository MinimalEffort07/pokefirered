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
    count = len(snapshot.items)
    header = 'board #%d "%s": %d item%s' % (
        snapshot.number, snapshot.title, count, "" if count == 1 else "s")
    if snapshot.skipped:
        header += ", %d other card%s skipped" % (
            snapshot.skipped, "" if snapshot.skipped == 1 else "s")
    print(header, file=log)
    for item in snapshot.items:
        print("#%d %s %s has_parent=%s" % (
            item.number, item.author, item.status,
            "yes" if item.has_parent else "no"), file=log)
    return 0
