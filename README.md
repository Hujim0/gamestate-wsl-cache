# WSL game state cache

One small C process polls the Windows detector snapshot every two seconds and
atomically replaces a regular Linux `~/.gamestatus` file. Request handlers can
read Linux storage without crossing `/mnt/c` on every request. No dependencies
beyond libc; no persistent shell, network listener, or log polling.

This replaces the earlier WSL log reader. The
[Windows game state detector](https://github.com/Hujim0/gamemode-activator)
remains the source of truth. Measured mount reads on ellis were about 4.5 ms,
versus 0.003 ms for Linux storage; these are host-specific measurements.

## Build and run

Build on the target Linux machine (a locally built executable may require a
newer libc than the WSL installation):

```sh
make
# Strict build:
make clean
make CFLAGS='-Os -std=c11 -Wall -Wextra -Wpedantic -Werror'
strip gamestatus
./gamestatus /mnt/c/Users/Ellis/AppData/Local/GameModeActivator/gamestatus \
  "$HOME/.gamestatus"
```

Arguments: `SOURCE CACHE [--hook SCRIPT]`. Use absolute paths in
services. Polling is fixed at two seconds; source freshness is 90 seconds.
Do not use the same file for source and cache. Point SOURCE directly at the
Windows file, not at the `~/.gamestatus` symlink being replaced.

The output directory must exist. A lock beside CACHE prevents duplicate writers.
An existing output symlink is atomically replaced with a regular local file;
its Windows target is untouched. Keep CACHE on Linux storage.

```ini
active=true
game="ZenlessZoneZero"
updated_at=1792612153
cached_at=1792612154
```

`updated_at` preserves the Windows timestamp; `cached_at` records the successful
local poll. The cache is refreshed every two seconds, even when unchanged.
Missing, malformed, oversized, stale, or future-dated source snapshots become
`active=unknown`, an empty game title, and `updated_at=0`. The parser accepts the
three Windows fields exactly once, with LF or CRLF line endings, and decodes the
producer's quoted title format. Unknown extra fields are rejected.

Consumers must parse the file as data, never source it. Check `cached_at` too:
if it is in the future or older than 10 seconds, the cache process may have
stopped or stalled; treat it as unknown. For known states also check that
`updated_at` is not in the future or older than 90 seconds. Reopen on each read
to see atomic replacements. Graceful shutdown publishes unknown; a force kill
leaves the previous cache, so freshness checks are still necessary. Inotify can
watch the *Linux directory* for replacements if a consumer prefers notifications.

## Events and broadcasts

The supplied `on-change.sh` hook runs `wall` when the active state changes,
including startup. Messages report active, inactive/ended, or unavailable state.
Title-only changes do not broadcast. `wall` may omit SSH pseudo-terminals that
WSL does not list in utmp, so the hook also writes to active interactive shell
terminals it finds. On ellis, interactive Bash and Fish startup enable terminal
write permission for `wall` recipients. This is not a Windows desktop
notification. There are no resource limits yet, so broadcasts do not claim
resources have been limited.
Broadcasting and future service controls live entirely in the hook; the binary
only caches state and dispatches events.

`--hook /absolute/path/on-change.sh` invokes:

```text
/bin/sh SCRIPT CURRENT_STATE GAME_TITLE PREVIOUS_DISPATCHED_STATE
```

Arguments are passed directly, never interpolated into shell code. State is
`true`, `false`, or `unknown`; previous state is empty at startup. Hooks run on
startup, state transitions, and changes to the active game's title. Heartbeats
do not run hooks. Use quoted arguments inside scripts. The supplied
`on-change.sh` broadcasts state and has placeholders for future service controls.

One short-lived worker runs the script. Cache polling
continues independently. Workers are serialized and have a 30-second total
timeout; background descendants in their process group are killed when the
worker exits. Do not daemonize from hooks; ask a service manager to manage
long-running work instead. Output/errors go to the service journal. Idle cache
polling does not log.

If state changes during a hook, only the latest observed state is dispatched
when it finishes. Intermediate transitions can be coalesced. Previous state
means last *dispatched*, not last successfully applied. Failed hooks are reported
but not retried until another transition or restart. Write idempotent scripts
that reconcile the current state, and do not assume every transition arrives.
The cache is published before the event starts: it describes detection, not
successful resource restriction. Shutdown cancels workers and writes unknown;
it does not run a final hook or automatically restore services.

There is no debounce yet. Alt-tab can generate transitions. Add stable policy
and recovery before using hooks to stop inference services. Unknown state is
not equivalent to inactive, and automatic restoration policy is still undecided.

## Install in WSL

Edit the Windows profile path in `gamestatus.service` for your installation.
The example enables the hook, which handles broadcasts. To install after building in WSL:

```sh
systemctl --user disable --now gamestatus.service
install -Dm755 gamestatus "$HOME/.local/bin/gamestatus"
mkdir -p "$HOME/.config/gamestatus"
# First install only; preserve your customized hook on upgrades:
if [ ! -e "$HOME/.config/gamestatus/on-change.sh" ]; then
  install -m644 on-change.sh "$HOME/.config/gamestatus/on-change.sh"
fi
install -Dm644 gamestatus.service "$HOME/.config/systemd/user/gamestatus.service"
systemctl --user daemon-reload
systemctl --user enable --now gamestatus.service
cat "$HOME/.gamestatus"
```

The old symlink is replaced on the first successful poll. The Windows file stays
in place. User systemd must be enabled in WSL. Lingering can keep the user service
running after logout while the WSL instance remains running. It does not start
Windows or WSL itself. Inspect with `systemctl --user status gamestatus` and
`journalctl --user -u gamestatus`.

To return to direct mount reads, stop/disable the service, move the cache aside,
and recreate the symlink using the
[detector README](https://github.com/Hujim0/gamemode-activator). Never run the
old log reader and this cache against the same output.

## Footprint

Fixed-size buffers and libc only; the process sleeps between polls. Shell and
`wall` processes exist only while handling an event. Actual RSS includes shared
libc pages and depends on the target system. The old log reader's memory numbers
do not measure this implementation. No model unload or service stop action is
enabled by the supplied hook.

Local validation on CachyOS: GCC built with `-Werror` and zero warnings;
integration checks covered symlink migration without changing the Windows source,
quoted hook arguments, duplicate-writer locking, transitions, heartbeat suppression,
missing/malformed/stale/future snapshots, recovery, graceful shutdown, and a hung
hook timing out while polling continued. Broadcast dispatch was checked with a
mock `wall`. A later delivery check found SSH shells missing from WSL utmp, so
the hook now writes to active interactive shell terminals as a fallback. The
process measured about 2.4 MiB RSS locally.
The systemd unit requires the binary and hook to be installed at its configured
paths before it can be run. This version is installed and enabled on ellis. The
local cache heartbeat and Zenless Zone Zero state were verified. RSS measured
about 2.1 MiB on WSL; the journal showed no hook errors. A terminal with messages
disabled caused `wall` to skip delivery despite a zero exit status. Interactive
Bash and Fish now enable terminal write permission; a direct broadcast check
confirmed the logged-in terminal is writable.
