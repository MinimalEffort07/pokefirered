#!/usr/bin/env bash
#
# Autodev setup: every step that can be scripted (ticket #53).
#
# Run automation/wizard.sh first (it creates the bot and its token), then
# this. Each step checks the current state and changes only what's missing,
# so running it again is safe. The second run should end with "no changes".
#
# Two GitHub identities are used:
#   owner_gh  your own gh login (owns the repo and the board)
#   bot_gh    the bot, using GH_TOKEN from ~/.config/autodev/env

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# Owner, repo and board title come from the dispatcher's own config, so
# the two can't disagree.
config() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$HERE/config.json" "$1"; }
OWNER=$(config owner)
REPO=$(config repo)
BOARD_TITLE=$(config board_title)
MAIN_CHECKOUT="$HOME/projects/minimaleffort/pokefirered"
CLONE="$HOME/projects/minimaleffort/pokefirered-autodev"
ENV_FILE="$HOME/.config/autodev/env"
UNIT_DIR="$HOME/.config/systemd/user"
RECORDINGS_PORT=8080

# Status options, in board order: name|colour|description (see #48).
STATUSES=(
  "Todo|GRAY|Waiting for the dispatcher (a card with no Status counts as Todo)"
  "Grilling|PURPLE|Parent: auto-grill session running"
  "Blocked|ORANGE|Ticket: an issue under Blocked by is still open"
  "In progress|YELLOW|Parent: tickets in flight. Ticket: a session is running"
  "In review|BLUE|Ticket: PR open, waiting for /merge or feedback"
  "Needs human|RED|Parked after a failure; see the latest bot comment"
  "Paused|PINK|Stop switch: nothing new is picked up"
  "Done|GREEN|Closed"
)

CHANGES=0
ok()      { printf '  ✓ %s\n' "$1"; }
changed() { printf '  + %s\n' "$1"; CHANGES=$((CHANGES + 1)); }
fail()    { printf '  ✗ %s\n' "$1" >&2; exit 1; }

owner_gh() { env -u GH_TOKEN gh "$@"; }
bot_gh()   { GH_TOKEN="$BOT_TOKEN" gh "$@"; }

# ── Preconditions ─────────────────────────────────────────────────────────
echo "Checking prerequisites"
[[ -f "$ENV_FILE" ]] || fail "$ENV_FILE is missing: run automation/wizard.sh first"
[[ "$(stat -c %a "$ENV_FILE")" == 600 ]] || fail "$ENV_FILE must be mode 600 (chmod 600 it)"
# The env file holds KEY='value' lines (also read by systemd's EnvironmentFile).
BOT_TOKEN=$(env -i bash -c 'set -a; . "$1"; printf %s "$GH_TOKEN"' _ "$ENV_FILE")
BOT_LOGIN=$(bot_gh api user --jq .login 2>/dev/null) || fail "the bot's token doesn't work: re-run automation/wizard.sh"
[[ "$BOT_LOGIN" != "$OWNER" ]] || fail "GH_TOKEN in $ENV_FILE belongs to $OWNER, not a bot"
owner_gh project list --owner "$OWNER" --limit 1 >/dev/null 2>&1 \
  || fail "your gh login lacks the project scope: re-run automation/wizard.sh"
ok "bot is $BOT_LOGIN; your gh login can manage boards"

# ── Bot is a collaborator on the repo ─────────────────────────────────────
echo "Repo access"
if [[ "$(bot_gh api "repos/$REPO" --jq .permissions.push)" == true ]]; then
  ok "$BOT_LOGIN can push to $REPO"
else
  owner_gh api -X PUT "repos/$REPO/collaborators/$BOT_LOGIN" -f permission=push --silent
  # The invitation must be accepted by the invitee; we hold its token.
  invite=$(bot_gh api user/repository_invitations \
    --jq "[.[] | select(.repository.full_name == \"$REPO\") | .id][0] // empty")
  [[ -n "$invite" ]] || fail "invited $BOT_LOGIN, but found no invitation to accept: re-run setup"
  bot_gh api -X PATCH "user/repository_invitations/$invite" --silent
  changed "invited $BOT_LOGIN to $REPO with write access, and accepted as the bot"
fi

# ── Board ─────────────────────────────────────────────────────────────────
echo "Board"
BOARD=$(owner_gh project list --owner "$OWNER" --limit 100 --format json \
  --jq ".projects[] | select(.title == \"$BOARD_TITLE\") | .number")
if [[ -n "$BOARD" ]]; then
  ok "board #$BOARD \"$BOARD_TITLE\" exists"
else
  BOARD=$(owner_gh project create --owner "$OWNER" --title "$BOARD_TITLE" --format json --jq .number)
  changed "created board #$BOARD \"$BOARD_TITLE\""
fi
BOARD_ID=$(owner_gh project view "$BOARD" --owner "$OWNER" --format json --jq .id)

linked=$(owner_gh api graphql -f query='query($o:String!,$r:String!){repository(owner:$o,name:$r){projectsV2(first:50){nodes{number}}}}' \
  -f o="${REPO%/*}" -f r="${REPO#*/}" --jq ".data.repository.projectsV2.nodes[] | select(.number == $BOARD) | .number")
if [[ -n "$linked" ]]; then
  ok "board is linked to $REPO"
else
  owner_gh project link "$BOARD" --owner "$OWNER" --repo "$REPO" >/dev/null
  changed "linked board to $REPO"
fi

# Status field: replace its options with exactly STATUSES, in order. An
# option that already exists keeps its id, so cards keep their Status.
# Name, colour and description are all compared, so editing STATUSES
# above takes effect on the next run.
status_field=$(owner_gh api graphql -f query='query($id:ID!){node(id:$id){... on ProjectV2{
    field(name:"Status"){... on ProjectV2SingleSelectField{id options{id name color description}}}}}}' \
  -f id="$BOARD_ID" --jq '.data.node.field')
want_names=$(printf '%s\n' "${STATUSES[@]}" | cut -d'|' -f1)
have=$(python3 -c 'import json,sys; [print("|".join((o["name"], o["color"], o["description"]))) for o in json.load(sys.stdin)["options"]]' <<<"$status_field")
if [[ "$have" == "$(printf '%s\n' "${STATUSES[@]}")" ]]; then
  ok "Status options are: $(paste -sd, <<<"$want_names")"
else
  python3 - "$status_field" "${STATUSES[@]}" <<'PY' | owner_gh api graphql --input - --silent
import json, sys
field = json.loads(sys.argv[1])
existing = {o["name"]: o["id"] for o in field.get("options", [])}
options = []
for spec in sys.argv[2:]:
    name, color, description = spec.split("|", 2)
    option = {"name": name, "color": color, "description": description}
    if name in existing:
        option["id"] = existing[name]
    options.append(option)
print(json.dumps({
    "query": "mutation($id:ID!,$opts:[ProjectV2SingleSelectFieldOptionInput!]){"
             "updateProjectV2Field(input:{fieldId:$id,singleSelectOptions:$opts}){clientMutationId}}",
    "variables": {"id": field["id"], "opts": options},
}))
PY
  changed "set Status options to: $(paste -sd, <<<"$want_names")"
fi

can_edit=$(bot_gh api graphql -f query='query($o:String!,$n:Int!){user(login:$o){projectV2(number:$n){viewerCanUpdate}}}' \
  -f o="$OWNER" -F n="$BOARD" --jq '.data.user.projectV2.viewerCanUpdate' 2>/dev/null || echo false)
if [[ "$can_edit" == true ]]; then
  ok "$BOT_LOGIN can edit the board"
else
  bot_id=$(bot_gh api user --jq .node_id)
  owner_gh api graphql --silent \
    -f query='mutation($p:ID!,$u:ID!){updateProjectV2Collaborators(input:{projectId:$p,collaborators:[{userId:$u,role:WRITER}]}){clientMutationId}}' \
    -f p="$BOARD_ID" -f u="$bot_id"
  changed "made $BOT_LOGIN an editor of the board"
fi

# ── Master rulesets ───────────────────────────────────────────────────────
# Two rulesets, because a ruleset's bypass list covers all of its rules:
#   "master history": no force-pushes, no deleting master; nobody bypasses.
#   "master via PR":  changes reach master only through a PR; admins (you)
#                     bypass, the bot (write access) cannot.
echo "Master rulesets"
ruleset_fallback() {
  printf '  ✗ could not create the master rulesets via the API (%s).\n' "$1" >&2
  printf '    Run automation/wizard.sh again; its last stage creates them by hand.\n' >&2
  exit 1
}
rulesets=$(owner_gh api "repos/$REPO/rulesets" --jq '.[].name' 2>&1) || ruleset_fallback "$rulesets"
create_ruleset() {
  local name="$1" body="$2" err
  if grep -qxF "$name" <<<"$rulesets"; then ok "ruleset \"$name\" exists"; return; fi
  if err=$(owner_gh api -X POST "repos/$REPO/rulesets" --input - --silent <<<"$body" 2>&1); then
    changed "created ruleset \"$name\""
  else
    ruleset_fallback "$err"
  fi
}
create_ruleset "autodev: master history" '{
  "name": "autodev: master history", "target": "branch", "enforcement": "active",
  "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
  "rules": [{"type": "deletion"}, {"type": "non_fast_forward"}],
  "bypass_actors": []}'
# actor_id 5 with actor_type RepositoryRole is GitHub's fixed id for the
# repository "Admin" role, so the bypass list means "admins of this repo".
create_ruleset "autodev: master via PR" '{
  "name": "autodev: master via PR", "target": "branch", "enforcement": "active",
  "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
  "rules": [{"type": "pull_request", "parameters": {
    "required_approving_review_count": 0, "dismiss_stale_reviews_on_push": false,
    "require_code_owner_review": false, "require_last_push_approval": false,
    "required_review_thread_resolution": false}}],
  "bypass_actors": [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]}'

# ── Dedicated clone ───────────────────────────────────────────────────────
# The automation never works in your own checkout. Its clone pushes over
# HTTPS, and git asks the credential helper below for a password. The
# helper loads the bot's token from the env file itself, so a push from
# this clone is always the bot's, even from a shell where GH_TOKEN is
# unset (gh would otherwise fall back to your own login).
echo "Dedicated clone"
if [[ -d "$CLONE/.git" ]]; then
  ok "$CLONE exists"
else
  git clone --quiet "https://github.com/$REPO.git" "$CLONE"
  changed "cloned $REPO into $CLONE"
fi
set_git() {
  if [[ "$(git -C "$CLONE" config --local --get-all "$1" 2>/dev/null | paste -sd'\n')" != "$2" ]]; then
    git -C "$CLONE" config --local --unset-all "$1" 2>/dev/null || true
    while IFS= read -r value; do git -C "$CLONE" config --local --add "$1" "$value"; done <<<"$2"
    changed "clone: $1"
  fi
}
bot_id_num=$(bot_gh api user --jq .id)
set_git remote.origin.url "https://github.com/$REPO.git"
# An empty helper first clears any helper inherited from ~/.gitconfig.
# git appends the action ("get", "store", ...) as the helper's argument.
set_git credential.helper "$(printf '\n%s' "!sh -c 'set -a; . \"$ENV_FILE\"; exec gh auth git-credential \"\$@\"' autodev-bot")"
set_git user.name "$BOT_LOGIN"
set_git user.email "$bot_id_num+$BOT_LOGIN@users.noreply.github.com"
if [[ "$(readlink "$CLONE/tools/agbcc" 2>/dev/null)" != "$MAIN_CHECKOUT/tools/agbcc" ]]; then
  ln -sfn "$MAIN_CHECKOUT/tools/agbcc" "$CLONE/tools/agbcc"
  changed "clone: tools/agbcc → $MAIN_CHECKOUT/tools/agbcc"
fi
ok "clone configured (pushes as $BOT_LOGIN over HTTPS)"
# The dispatcher runs whatever the clone has checked out. Setup never
# switches branches itself (someone may be testing a branch there), but
# it says so when the clone isn't on master.
clone_branch=$(git -C "$CLONE" branch --show-current)
[[ "$clone_branch" == master ]] \
  || printf '  ! the clone is on branch %s, not master: the dispatcher runs that branch\n' "$clone_branch"

# ── systemd user units ────────────────────────────────────────────────────
echo "systemd units"
mkdir -p "$UNIT_DIR"
units_changed=0
install_unit() {
  local name="$1" content="$2"
  if [[ -f "$UNIT_DIR/$name" && "$(cat "$UNIT_DIR/$name")" == "$content" ]]; then
    ok "$name is current"
  else
    printf '%s\n' "$content" > "$UNIT_DIR/$name"
    units_changed=1
    changed "installed $name"
  fi
}
install_unit autodev-dispatcher.service "[Unit]
Description=Autodev dispatcher: one tick (see automation/README.md)

[Service]
Type=oneshot
# A oneshot has no start time limit by default; a tick stuck on the network
# would then block every later tick.
TimeoutStartSec=5min
EnvironmentFile=$ENV_FILE
WorkingDirectory=$CLONE/automation
# Bring the clone up to date first, so merged dispatcher changes take
# effect. The leading '-' means a failed pull (e.g. offline) still ticks.
ExecStartPre=-/usr/bin/git -C $CLONE pull --ff-only --quiet
ExecStart=/usr/bin/python3 -m autodev tick"
install_unit autodev.timer "[Unit]
Description=Run the autodev dispatcher every 2 minutes

[Timer]
# First tick a minute after the user's systemd starts, then every 2 minutes.
# systemd may otherwise shift a timer by up to a minute to batch wakeups;
# 10 s keeps ticks close to 2 minutes apart.
OnBootSec=1min
OnUnitActiveSec=2min
AccuracySec=10s
Unit=autodev-dispatcher.service

[Install]
WantedBy=timers.target"
install_unit autodev-recordings.service "[Unit]
Description=mGBA test recordings server on port $RECORDINGS_PORT

[Service]
WorkingDirectory=$CLONE
ExecStart=/usr/bin/python3 test/serve_recordings.py
Restart=on-failure

[Install]
WantedBy=default.target"
(( units_changed )) && systemctl --user daemon-reload

if systemctl --user is-enabled --quiet autodev.timer && systemctl --user is-active --quiet autodev.timer; then
  ok "autodev.timer is enabled and running"
else
  systemctl --user enable --now autodev.timer
  changed "enabled and started autodev.timer"
fi

# The recordings server was hand-started before autodev; two servers can't
# share the port, so a hand-started one has to be stopped first.
if systemctl --user is-enabled --quiet autodev-recordings.service \
    && systemctl --user is-active --quiet autodev-recordings.service; then
  ok "autodev-recordings.service is enabled and running"
else
  holder=$(ss -ltnpH "sport = :$RECORDINGS_PORT" | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
  if [[ -n "$holder" ]]; then
    fail "port $RECORDINGS_PORT is held by pid $holder ($(ps -o args= -p "$holder")). Stop it, then re-run setup."
  fi
  systemctl --user enable --now autodev-recordings.service
  changed "enabled and started autodev-recordings.service"
fi

# ── Linger ────────────────────────────────────────────────────────────────
# Without linger, systemd stops a user's units when their last login
# session ends. With it, the timer keeps ticking with no terminal open.
echo "Linger"
if [[ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" == yes ]]; then
  ok "linger is enabled for $USER"
elif loginctl enable-linger "$USER" 2>/dev/null; then
  changed "enabled linger for $USER"
else
  fail "couldn't enable linger: run 'sudo loginctl enable-linger $USER', then re-run setup"
fi

echo
if (( CHANGES == 0 )); then echo "setup: no changes"; else echo "setup: $CHANGES change(s)"; fi
