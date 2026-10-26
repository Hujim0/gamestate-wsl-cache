#!/bin/sh
# Arguments are data: current state, game title, previous dispatched state.
# Called at startup too (previous state is empty). Keep actions idempotent.
set -eu
state=$1
game=$2
previous=$3

# WSL's utmp can omit SSH pseudo-terminals, so wall may report success while
# sending to none of the user's interactive SSH shells. Use wall for sessions
# it knows about, then write directly to live shell terminals it missed.
broadcast() {
    message=$1
    wall "$message" || :

    logged_terminals=$(who 2>/dev/null | awk '$2 ~ /^pts\/[0-9]+$/ { print $2 }')
    ps -eo tty=,comm= |
        awk '$1 ~ /^pts\/[0-9]+$/ && $2 ~ /^(bash|fish|zsh|sh)$/ { print $1 }' |
        sort -u |
        while IFS= read -r terminal; do
            if printf '%s\n' "$logged_terminals" | grep -Fxq "$terminal"; then
                continue
            fi
            printf '\r\n%s\r\n' "$message" > "/dev/$terminal" 2>/dev/null || :
        done
}

# A title change does not require another broadcast or service transition.
[ "$state" != "$previous" ] || exit 0

case "$state" in
    true)
        # Future: stop managed inference services here, before announcing limits.
        broadcast 'Game mode is active.'
        ;;
    false)
        # Future: restore managed services here, according to the recovery policy.
        if [ -z "$previous" ]; then
            broadcast 'Game mode is inactive.'
        else
            broadcast 'Game mode ended.'
        fi
        ;;
    unknown)
        # Missing/stale/invalid source does not mean the game ended.
        broadcast 'Game mode status is unavailable.'
        ;;
esac
