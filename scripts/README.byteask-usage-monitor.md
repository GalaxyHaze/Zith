# ByteAsk Usage Monitor

Small persistent Python process that reads the active ByteAsk account token from
`~/.byteask/config.toml`, queries the usage endpoint, stores snapshots locally,
and sends desktop notifications when usage crosses configurable thresholds.

It does not modify `config.toml`, switch accounts, or touch the ByteAsk binary.

## Install And Run

The source of truth is `scripts/byteask-usage-monitor.py`. Copy it to the user
bin with the public command name and install a graphical-session autostart entry:

```bash
install -m 0755 scripts/byteask-usage-monitor.py ~/.local/bin/byteask-usage-monitor
install -m 0644 scripts/byteask-usage-monitor.desktop ~/.config/autostart/byteask-usage-monitor.desktop
byteask-usage-monitor start
```

The daemon reads `~/.byteask/config.toml` and `~/.byteask/gateway` on every
check, so re-login that updates those files does not require a restart.

## Commands

- `byteask-usage-monitor status` prints live usage and the state file path.
- `byteask-usage-monitor once` runs one check and exits; it may notify.
- `byteask-usage-monitor accounts` lists known accounts, active state, and stored snapshots.
- `byteask-usage-monitor refresh --account EMAIL` refreshes a previously tracked account using its stored local credential.
- `byteask-usage-monitor refresh-all` refreshes every tracked account with a stored credential.
- `byteask-usage-monitor start|stop|status-daemon` manages the resident loop.
- `byteask-usage-monitor notify-test` sends a test desktop notification.
- `byteask-usage-monitor self-test` runs offline loading/parsing/notification tests.

## Configuration

Optional JSON at `~/.byteask-usage-monitor/config.json`:

```json
{
  "poll_seconds": 60,
  "thresholds": [70, 80, 90],
  "cooldown_minutes": 60
}
```

`poll_seconds` is clamped to at least 5. `thresholds` are sorted and applied to
both the 5-hour and weekly usage percentages. Once a level is notified, it is not
repeated while usage stays above that level; after usage drops below it, the
level can fire again. `cooldown_minutes` gates re-notification after activity
and is disabled when set to 0.

State and PID files live under `~/.byteask-usage-monitor/` with restrictive
permissions. The token is only sent to the configured gateway usage endpoint.

When the monitor runs while a ByteAsk account is active, it stores a local
credential for that email under `credentials.json` (`0600`) and records usage
snapshots in `state.json`. Accounts from `~/.byteask/.known-emails` appear in
`accounts` even before they are active, but only emails that have been active
while the monitor was running can be refreshed afterward. The monitor never
switches the active ByteAsk account by itself.
