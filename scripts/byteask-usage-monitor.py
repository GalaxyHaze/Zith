#!/usr/bin/env python3
"""ByteAsk usage monitor.

Reads the active ByteAsk account token from ~/.byteask/config.toml and queries
the gateway usage endpoint. It stays resident as a tiny process and sends
desktop notifications when usage passes configurable percentage thresholds.
"""

from __future__ import annotations

import base64
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Callable, Optional

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    tomllib = None


PROG = "byteask-usage-monitor"
VERSION = "1.0.0"
DEFAULT_GATEWAY = "https://code.byteask.ai"
DEFAULT_POLL_SECONDS = 60
DEFAULT_THRESHOLDS = [70, 80, 90]
DEFAULT_COOLDOWN_MINUTES = 60
REQUEST_TIMEOUT_SECONDS = 10
NOTIFICATION_TIMEOUT_SECONDS = 10


class UsageError(Exception):
    pass


def home_dir() -> Path:
    return Path.home()


def monitor_dir(home: Path) -> Path:
    return home / ".byteask-usage-monitor"


def state_path(home: Path) -> Path:
    return monitor_dir(home) / "state.json"


def credentials_path(home: Path) -> Path:
    return monitor_dir(home) / "credentials.json"


def pid_path(home: Path) -> Path:
    return monitor_dir(home) / f"byteask-usage-monitor.{os.getuid()}.pid"


def daemon_log_path(home: Path) -> Path:
    return monitor_dir(home) / "daemon.log"


def config_path(home: Path) -> Path:
    return monitor_dir(home) / "config.json"


def byteask_home(home: Path) -> Path:
    return home / ".byteask"


def _coerce_float(value: Any) -> Optional[float]:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _coerce_int(value: Any, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _coerce_bool(value: Any, default: bool) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on")
    return default


def load_runtime_config(home: Path) -> dict[str, Any]:
    cfg: dict[str, Any] = {
        "poll_seconds": DEFAULT_POLL_SECONDS,
        "thresholds": list(DEFAULT_THRESHOLDS),
        "cooldown_minutes": DEFAULT_COOLDOWN_MINUTES,
        "track_all_known_emails": True,
    }
    path = config_path(home)
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return cfg
    except (json.JSONDecodeError, OSError) as exc:
        raise UsageError(f"cannot read {path}: {exc}") from exc

    if not isinstance(raw, dict):
        raise UsageError(f"{path} must contain a JSON object")

    if "poll_seconds" in raw:
        cfg["poll_seconds"] = max(5, _coerce_int(raw["poll_seconds"], DEFAULT_POLL_SECONDS))
    if "cooldown_minutes" in raw:
        cfg["cooldown_minutes"] = max(0, _coerce_int(raw["cooldown_minutes"], DEFAULT_COOLDOWN_MINUTES))
    if "thresholds" in raw and isinstance(raw["thresholds"], list):
        parsed: list[int] = []
        for entry in raw["thresholds"]:
            numeric = _coerce_int(entry, 0)
            if numeric > 0:
                parsed.append(numeric)
        if parsed:
            cfg["thresholds"] = sorted(set(parsed))
    if "track_all_known_emails" in raw:
        cfg["track_all_known_emails"] = _coerce_bool(raw["track_all_known_emails"], True)
    return cfg


def _simple_toml_load(text: str) -> dict[str, Any]:
    """Tiny fallback for systems without tomllib; only the keys we need."""
    result: dict[str, Any] = {}
    current_section: dict[str, Any] = result
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        section = re.match(r"^\[([^\]]+)\]$", stripped)
        if section:
            current_section = result
            for part in section.group(1).split("."):
                current_section = current_section.setdefault(part, {})
            continue
        match = re.match(r'^([A-Za-z0-9_.-]+)\s*=\s*"((?:\\.|[^"\\])*)"\s*$', stripped)
        if match:
            key, value = match.group(1), match.group(2)
            value = value.replace('\\"', '"').replace("\\\\", "\\")
            current_section[key] = value
    return result


def load_auth(home: Path = None) -> dict[str, str]:
    """Load active gateway and token without mutating ByteAsk's config.toml."""
    if home is None:
        home = home_dir()
    config = byteask_home(home) / "config.toml"
    if not config.is_file():
        raise UsageError(f"missing ByteAsk config: {config}")

    try:
        text = config.read_text(encoding="utf-8")
        if tomllib is not None:
            data = tomllib.loads(text)
        else:
            data = _simple_toml_load(text)
    except (OSError, ValueError) as exc:
        raise UsageError(f"cannot parse ByteAsk config {config}: {exc}") from exc

    provider = data.get("model_providers", {})
    if not isinstance(provider, dict):
        provider = {}
    byteask_provider = provider.get("byteask", {})
    if not isinstance(byteask_provider, dict):
        byteask_provider = {}

    token = byteask_provider.get("experimental_bearer_token")
    if isinstance(token, str):
        token = token.strip()
    if not token:
        raise UsageError(f"no experimental_bearer_token found in {config}")

    gateway = ""
    gateway_file = byteask_home(home) / "gateway"
    if gateway_file.is_file():
        gateway = gateway_file.read_text(encoding="utf-8").strip()
    if not gateway:
        base_url = byteask_provider.get("base_url")
        if isinstance(base_url, str):
            candidate = base_url.strip().rstrip("/")
            if candidate.endswith("/byteask/v1"):
                candidate = candidate[: -len("/byteask/v1")]
            if candidate:
                gateway = candidate
    if not gateway:
        gateway = DEFAULT_GATEWAY

    return {
        "token": token,
        "gateway": gateway.rstrip("/"),
        "email": decode_email_from_token(token),
    }


def decode_email_from_token(token: str) -> Optional[str]:
    if not token:
        return None
    try:
        chunk = token.split(".")[1]
        chunk += "=" * (-len(chunk) % 4)
        payload = json.loads(base64.urlsafe_b64decode(chunk).decode("utf-8"))
        subject = payload.get("sub")
        return str(subject) if subject else None
    except (IndexError, ValueError, TypeError, json.JSONDecodeError, UnicodeDecodeError):
        return None


def usage_url(gateway: str) -> str:
    return f"{gateway.rstrip('/')}/byteask/v1/usage"


def fetch_usage(gateway: str, token: str, timeout: float = REQUEST_TIMEOUT_SECONDS) -> dict[str, Any]:
    request = urllib.request.Request(
        usage_url(gateway),
        method="GET",
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/json",
            "User-Agent": f"{PROG}/{VERSION}",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace").strip()[:300]
        raise UsageError(f"usage endpoint returned HTTP {exc.code}: {detail}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise UsageError(f"usage endpoint request failed: {exc}") from exc

    try:
        parsed = json.loads(body)
    except json.JSONDecodeError as exc:
        raise UsageError(f"usage endpoint returned invalid JSON: {exc}") from exc
    if not isinstance(parsed, dict):
        raise UsageError("usage endpoint returned a non-object payload")
    return parsed


def parse_usage(payload: dict[str, Any]) -> dict[str, Any]:
    usage: dict[str, Any] = {
        "used_5h_pct": _coerce_float(payload.get("used_5h_pct")),
        "used_week_pct": _coerce_float(payload.get("used_week_pct")),
        "reset_5h_sec": _coerce_float(payload.get("reset_5h_sec")),
        "reset_week_sec": _coerce_float(payload.get("reset_week_sec")),
        "plan": str(payload["plan"]) if payload.get("plan") is not None else None,
        "raw": payload,
    }
    return usage


def load_state(home: Path) -> dict[str, Any]:
    path = state_path(home)
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {"notified": {}, "last_snapshot": None, "last_active": None, "accounts": {}}
    except (json.JSONDecodeError, OSError):
        return {"notified": {}, "last_snapshot": None, "last_active": None, "accounts": {}}
    if not isinstance(raw, dict):
        return {"notified": {}, "last_snapshot": None, "last_active": None, "accounts": {}}
    notified = raw.get("notified")
    if not isinstance(notified, dict):
        notified = {}
    accounts = raw.get("accounts")
    if not isinstance(accounts, dict):
        accounts = {}
    normalized_accounts: dict[str, Any] = {}
    for email in sorted(str(k) for k in accounts):
        entry = accounts[email]
        if not isinstance(entry, dict):
            entry = {}
        normalized_accounts[str(email)] = {
            "last_snapshot": entry.get("last_snapshot"),
            "first_seen": entry.get("first_seen"),
            "last_seen": entry.get("last_seen"),
            "token_refreshed_at": entry.get("token_refreshed_at"),
            "error": entry.get("error"),
        }
    return {
        "notified": notified,
        "last_snapshot": raw.get("last_snapshot"),
        "last_active": raw.get("last_active"),
        "accounts": normalized_accounts,
    }


def save_state(state: dict[str, Any], home: Path) -> None:
    directory = monitor_dir(home)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(directory, 0o700)
    path = state_path(home)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)
    os.chmod(path, 0o600)


def load_credentials(home: Path) -> dict[str, str]:
    path = credentials_path(home)
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}
    except (json.JSONDecodeError, OSError):
        return {}
    if not isinstance(raw, dict):
        return {}
    return {str(k): str(v) for k, v in raw.items() if isinstance(v, str) and v}


def save_credentials(credentials: dict[str, str], home: Path) -> None:
    directory = monitor_dir(home)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(directory, 0o700)
    path = credentials_path(home)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(credentials, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)
    os.chmod(path, 0o600)


def known_emails(home: Path) -> list[str]:
    path = byteask_home(home) / ".known-emails"
    try:
        lines = [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    except OSError:
        return []
    return sorted(set(lines))


def _metadata_for_account(account: dict[str, Any]) -> dict[str, Any]:
    return {
        "last_snapshot": account.get("last_snapshot"),
        "first_seen": account.get("first_seen"),
        "last_seen": account.get("last_seen"),
        "token_refreshed_at": account.get("token_refreshed_at"),
        "error": account.get("error"),
        "token_available": account.get("token_available", False),
    }


def _update_account_record(
    state: dict[str, Any],
    email: str,
    *,
    now: float,
    snapshot: Optional[dict[str, Any]] = None,
    token_refreshed_at: Optional[float] = None,
    error: Optional[str] = None,
) -> None:
    accounts = state.setdefault("accounts", {})
    account = accounts.get(email)
    if not isinstance(account, dict):
        account = {}
    if not account.get("first_seen"):
        account["first_seen"] = snapshot.get("at") if snapshot else time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))
    account["last_seen"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))
    if snapshot is not None:
        account["last_snapshot"] = snapshot
        account.pop("error", None)
        account["token_available"] = True
    if token_refreshed_at is not None:
        account["token_refreshed_at"] = token_refreshed_at
        account["token_available"] = True
    if error is not None:
        account["error"] = error
    accounts[email] = _metadata_for_account(account)


def _ensure_known_accounts(state: dict[str, Any], home: Path, cfg: dict[str, Any], now: float) -> None:
    if not cfg.get("track_all_known_emails", True):
        return
    accounts = state.setdefault("accounts", {})
    credentials = load_credentials(home)
    for email in known_emails(home):
        if email in accounts:
            continue
        account: dict[str, Any] = {
            "last_snapshot": None,
            "first_seen": None,
            "last_seen": None,
            "token_refreshed_at": None,
            "error": None,
            "token_available": email in credentials,
        }
        accounts[email] = _metadata_for_account(account)
    for email, token in credentials.items():
        if email and email not in accounts:
            account = {
                "last_snapshot": _account_snapshot_from_state(state, email),
                "first_seen": None,
                "last_seen": None,
                "token_refreshed_at": None,
                "error": None,
                "token_available": True,
            }
            accounts[email] = _metadata_for_account(account)


def _upsert_account_snapshot(
    state: dict[str, Any],
    auth: dict[str, str],
    usage: dict[str, Any],
    now: float,
    email: Optional[str] = None,
) -> None:
    selected_email = email or auth.get("email")
    if not selected_email:
        return
    snapshot = snapshot_from_response(auth, usage, now)
    _update_account_record(state, selected_email, now=now, snapshot=snapshot)


def _account_snapshot_from_state(state: dict[str, Any], email: str) -> Optional[dict[str, Any]]:
    entry = state.get("accounts", {}).get(email)
    if isinstance(entry, dict):
        return entry.get("last_snapshot")
    return None


def _notify_key(window: str, threshold: int) -> str:
    return f"{window}:{threshold}"


def _cooldown_seconds(cfg: dict[str, Any]) -> float:
    return float(cfg.get("cooldown_minutes", DEFAULT_COOLDOWN_MINUTES) * 60)


def _rearm_lowered(state: dict[str, Any], usage: dict[str, Any]) -> None:
    notified = state.setdefault("notified", {})
    windows = {
        "5h": usage.get("used_5h_pct"),
        "week": usage.get("used_week_pct"),
    }
    for key in list(notified):
        if ":" not in key:
            continue
        window, raw_threshold = key.split(":", 1)
        if window not in windows:
            continue
        try:
            threshold = int(raw_threshold)
        except ValueError:
            continue
        current = windows[window]
        if current is None or current < threshold:
            del notified[key]


def _notify_for_usage(
    usage: dict[str, Any],
    state: dict[str, Any],
    cfg: dict[str, Any],
    notify: Callable[[str, str], None],
    now: float,
) -> None:
    _rearm_lowered(state, usage)
    notified = state.setdefault("notified", {})
    thresholds = [int(t) for t in cfg.get("thresholds", DEFAULT_THRESHOLDS)]
    cooldown_seconds = _cooldown_seconds(cfg)
    last_active = state.get("last_active")
    last_active_ts = float(last_active) if isinstance(last_active, (int, float)) else 0.0

    for window, current in (("5h", usage.get("used_5h_pct")), ("week", usage.get("used_week_pct"))):
        if current is None:
            continue
        crossed = [threshold for threshold in thresholds if current >= threshold]
        if not crossed:
            continue
        cooldown_expired = cooldown_seconds <= 0 or last_active is None
        if last_active is not None:
            cooldown_expired = now - last_active_ts >= cooldown_seconds
        eligible = [
            threshold
            for threshold in crossed
            if _notify_key(window, threshold) not in notified
            and cooldown_expired
        ]
        if not eligible:
            continue
        chosen = max(eligible)
        state["notified"][_notify_key(window, chosen)] = now
        state["last_active"] = now
        notify(window, chosen)


def perform_tracked_refresh(home: Path, cfg: dict[str, Any], notify: bool = False) -> dict[str, Any]:
    """Refresh every stored account credential that is not active."""
    auth = load_auth(home)
    gateway = auth["gateway"]
    active_email = auth.get("email")
    credentials = load_credentials(home)
    state = load_state(home)
    _ensure_known_accounts(state, home, cfg, time.time())
    now = time.time()
    refreshed: dict[str, Any] = {"ok": [], "errors": []}
    errors: list[tuple[str, str]] = []
    for email, token in credentials.items():
        if not email or email == active_email:
            continue
        try:
            payload = fetch_usage(gateway, token, REQUEST_TIMEOUT_SECONDS)
            usage = parse_usage(payload)
            _update_account_record(
                state,
                email,
                now=now,
                snapshot=snapshot_from_response(
                    {"gateway": gateway, "token": token, "email": email},
                    usage,
                    now,
                ),
            )
            refreshed["ok"].append(email)
        except UsageError as exc:
            errors.append((email, str(exc)))
            _update_account_record(state, email, now=now, error=str(exc))
            refreshed["errors"].append({"email": email, "error": str(exc)})
    if errors:
        refreshed["errors"] = [{"email": email, "error": err} for email, err in errors]
    save_state(state, home)
    return refreshed


def notify_desktop(window: str, threshold: int) -> None:
    try:
        summary = "ByteAsk: usage high"
        body = f"{window} usage reached {threshold}%"
        subprocess.run(
            ["notify-send", "-a", PROG, "-u", "normal", "-t", "8000", summary, body],
            check=True,
            timeout=NOTIFICATION_TIMEOUT_SECONDS,
        )
    except FileNotFoundError:
        print("warning: notify-send is not installed", file=sys.stderr)
    except subprocess.SubprocessError as exc:
        print(f"warning: notify-send failed: {exc}", file=sys.stderr)


def snapshot_from_response(auth: dict[str, str], usage: dict[str, Any], now: float) -> dict[str, Any]:
    return {
        "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)),
        "email": auth.get("email"),
        "plan": usage.get("plan"),
        "used_5h_pct": usage.get("used_5h_pct"),
        "used_week_pct": usage.get("used_week_pct"),
        "reset_5h_sec": usage.get("reset_5h_sec"),
        "reset_week_sec": usage.get("reset_week_sec"),
    }


def perform_check(
    *,
    home: Path,
    cfg: dict[str, Any],
    notify: bool,
    now: Optional[float] = None,
    fetch: Callable[[str, str, float], dict[str, Any]] = fetch_usage,
    persist_credentials: bool = True,
) -> dict[str, Any]:
    if now is None:
        now = time.time()
    auth = load_auth(home)
    payload = fetch(auth["gateway"], auth["token"], REQUEST_TIMEOUT_SECONDS)
    usage = parse_usage(payload)
    state = load_state(home)
    state["last_snapshot"] = snapshot_from_response(auth, usage, now)
    _ensure_known_accounts(state, home, cfg, now)
    if auth.get("email"):
        _upsert_account_snapshot(state, auth, usage, now)

    if notify:
        _notify_for_usage(usage, state, cfg, notify_desktop, now)
    credentials = load_credentials(home)
    if auth.get("email") and auth["token"]:
        credentials[auth["email"]] = auth["token"]
        _update_account_record(
            state,
            auth["email"],
            now=now,
            token_refreshed_at=now,
        )
        if persist_credentials:
            save_credentials(credentials, home)
    save_state(state, home)

    result = {
        "auth": auth,
        "usage": usage,
        "state": state,
        "state_path": str(state_path(home)),
        "credentials": credentials,
    }
    return result


def format_percent(value: Optional[float]) -> str:
    if value is None:
        return "-"
    return f"{value:.1f}%"


def format_reset(seconds: Optional[float]) -> str:
    if seconds is None:
        return "-"
    if seconds <= 0:
        return "now"
    total = int(seconds)
    hours, remainder = divmod(total, 3600)
    minutes = remainder // 60
    if hours:
        return f"{hours}h {minutes:02d}m"
    return f"{minutes}m"


def format_credits(payload: dict[str, Any]) -> str:
    for key in ("credit_balance", "credits_remaining", "credits"):
        if payload.get(key) is not None:
            return str(payload[key])
    return "-"


def print_result(status: bool, result: dict[str, Any] | None, home: Path) -> None:
    if result is None:
        print("email: -")
        print("plan: -")
        print("5h: -")
        print("week: -")
        print("reset_5h: -")
        print("reset_week: -")
        print("state: -")
        return
    usage = result["usage"]
    auth = result["auth"]
    raw = usage.get("raw", {})
    print(f"email: {auth.get('email') or '-'}")
    print(f"plan: {usage.get('plan') or '-'}")
    print(f"5h: {format_percent(usage.get('used_5h_pct'))}")
    print(f"week: {format_percent(usage.get('used_week_pct'))}")
    print(f"reset_5h: {format_reset(usage.get('reset_5h_sec'))}")
    print(f"reset_week: {format_reset(usage.get('reset_week_sec'))}")
    print(f"credits: {format_credits(raw)}")
    print(f"active_email: {auth.get('email') or '-'}")
    tracked = len(state.get("accounts", {}))
    print(f"tracked_accounts: {tracked}")
    print(f"state: {state_path(home)}")
    if not status:
        print("notification warnings may appear on stderr", file=sys.stderr)


def process_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def read_pid(home: Path) -> Optional[int]:
    path = pid_path(home)
    try:
        raw = path.read_text(encoding="ascii").strip()
    except (FileNotFoundError, OSError, UnicodeDecodeError):
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def clear_stale_pid(home: Path, current_pid: Optional[int] = None) -> None:
    """Remove a PID file that does not point at a live process."""
    pid = read_pid(home)
    if pid is None:
        return
    if current_pid == pid:
        return
    if process_alive(pid):
        return
    try:
        pid_path(home).unlink()
    except FileNotFoundError:
        pass


def acquire_lock(home: Path) -> bool:
    directory = monitor_dir(home)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(directory, 0o700)
    path = pid_path(home)
    for _ in range(3):
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        except FileExistsError:
            pid = read_pid(home)
            if pid == os.getpid() or (pid is not None and process_alive(pid)):
                return False
            clear_stale_pid(home, os.getpid())
            continue
        with os.fdopen(fd, "w", encoding="ascii") as handle:
            handle.write(f"{os.getpid()}\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(path, 0o600)
        return True
    return False


def release_lock(home: Path) -> None:
    pid = read_pid(home)
    if pid is not None and pid == os.getpid():
        try:
            pid_path(home).unlink()
        except FileNotFoundError:
            pass


def run_loop(home: Path, cfg: dict[str, Any]) -> None:
    def handle_term(_signum: int, _frame: Any) -> None:
        release_lock(home)
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, handle_term)
    signal.signal(signal.SIGINT, handle_term)

    while True:
        try:
            perform_check(home=home, cfg=cfg, notify=True)
        except UsageError as exc:
            print(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} {exc}", file=sys.stderr)
        except OSError as exc:
            print(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} os error: {exc}", file=sys.stderr)
        try:
            perform_tracked_refresh(home, cfg)
        except UsageError as exc:
            print(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} tracked refresh: {exc}", file=sys.stderr)
        except OSError as exc:
            print(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} tracked refresh os error: {exc}", file=sys.stderr)
        time.sleep(cfg.get("poll_seconds", DEFAULT_POLL_SECONDS))


def cmd_status(args: list[str], home: Path) -> int:
    cfg = load_runtime_config(home)
    try:
        result = perform_check(home=home, cfg=cfg, notify=False)
        print_result(True, result, home)
        return 0
    except UsageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        try:
            last = load_state(home).get("last_snapshot")
            if last:
                print("last snapshot:", file=sys.stderr)
                print(json.dumps(last, indent=2), file=sys.stderr)
        except OSError:
            pass
        return 1


def cmd_once(args: list[str], home: Path) -> int:
    cfg = load_runtime_config(home)
    try:
        result = perform_check(home=home, cfg=cfg, notify=True)
        print_result(True, result, home)
        return 0
    except UsageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


def cmd_daemon(args: list[str], home: Path) -> int:
    if not acquire_lock(home):
        print("error: another instance owns the PID file", file=sys.stderr)
        return 1
    cfg = load_runtime_config(home)
    try:
        run_loop(home, cfg)
    finally:
        release_lock(home)
    return 0


def cmd_start(args: list[str], home: Path) -> int:
    let = os.environ.get("PATH", "")
    if not shutil_which_prologue("byteask-usage-monitor", let):
        print("warning: this script is not on PATH as byteask-usage-monitor", file=sys.stderr)

    clear_stale_pid(home)
    existing_pid = read_pid(home)
    if existing_pid is not None and process_alive(existing_pid):
        print(f"already running: pid={existing_pid}")
        print(f"state: {state_path(home)}")
        return 0

    script = os.path.realpath(sys.argv[0])
    log = daemon_log_path(home)
    log.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with open(log, "a", encoding="utf-8") as log_handle:
        handle = subprocess.Popen(
            [sys.executable, script, "daemon"],
            stdin=subprocess.DEVNULL,
            stdout=log_handle,
            stderr=log_handle,
            start_new_session=True,
            close_fds=True,
        )
    deadline = time.time() + 3.0
    while time.time() < deadline:
        time.sleep(0.15)
        if read_pid(home) is not None:
            break
        if handle.poll() is not None:
            print(f"error: daemon exited with status {handle.returncode}", file=sys.stderr)
            return 1
    final_pid = read_pid(home)
    if final_pid is None or not process_alive(final_pid):
        print("error: daemon did not start (checking log `status-daemon` and daemon.log)", file=sys.stderr)
        return 1

    print(f"started: pid={final_pid}")
    print(f"state: {state_path(home)}")
    print(f"log: {log}")
    return 0


def shutil_which_prologue(name: str, path: str) -> bool:
    # Kept as a small helper to avoid importing shutil for just one call.
    for directory in path.split(os.pathsep):
        if not directory:
            continue
        candidate = Path(directory) / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return True
    return False


def cmd_stop(args: list[str], home: Path) -> int:
    pid = read_pid(home)
    if pid is None or not process_alive(pid):
        if pid_path(home).exists():
            try:
                pid_path(home).unlink()
            except OSError:
                pass
        print("not running")
        return 0
    os.kill(pid, signal.SIGTERM)
    deadline = time.time() + 8
    while time.time() < deadline:
        if not process_alive(pid):
            break
        time.sleep(0.25)
    if process_alive(pid):
        print(f"error: pid {pid} did not stop after SIGTERM", file=sys.stderr)
        return 1
    if pid_path(home).exists():
        try:
            pid_path(home).unlink()
        except OSError:
            pass
    print(f"stopped: pid={pid}")
    return 0


def cmd_status_daemon(args: list[str], home: Path) -> int:
    pid = read_pid(home)
    if pid is not None and process_alive(pid):
        print(f"running: pid={pid}")
        print(f"state: {state_path(home)}")
        print(f"log: {daemon_log_path(home)}")
        return 0
    print("not running")
    return 1


def cmd_notify_test(args: list[str], home: Path) -> int:
    try:
        notify_desktop("test", 0)
    except UsageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print("notification sent")
    return 0


def _email_display(email: str) -> str:
    return email or "-"


def _account_status(entry: Optional[dict[str, Any]], active_email: Optional[str], email: str) -> str:
    if email == active_email:
        return "active"
    token_available = bool(entry and entry.get("token_available"))
    if entry and entry.get("last_snapshot"):
        return "tracked" if token_available else "cached"
    return "tracked" if token_available else "known"


def cmd_accounts(args: list[str], home: Path) -> int:
    cfg = load_runtime_config(home)
    try:
        active_auth = load_auth(home)
        active_email = active_auth.get("email")
    except UsageError:
        active_email = None
    state = load_state(home)
    accounts = state.get("accounts", {})
    _ensure_known_accounts(state, home, cfg, time.time())
    accounts = state.get("accounts", {})
    if active_email and active_email not in accounts:
        accounts[active_email] = {
            "last_snapshot": None,
            "first_seen": None,
            "last_seen": None,
            "token_refreshed_at": None,
            "error": None,
            "token_available": True,
        }
    credentials = load_credentials(home)
    emails: set[str] = set()
    emails.update(str(e) for e in accounts)
    emails.update(credentials)
    if cfg.get("track_all_known_emails", True):
        emails.update(known_emails(home))
    emails.discard("")
    if not emails:
        print("no accounts known")
        return 0

    print(f"tracking: {len(emails)} account(s)")
    print("email | state | 5h | week | reset_5h | reset_week | last_seen")
    for email in sorted(emails, key=lambda item: (item != active_email, item.lower())):
        entry = accounts.get(email)
        snapshot = None
        if isinstance(entry, dict):
            snapshot = entry.get("last_snapshot")
        if not isinstance(snapshot, dict):
            snapshot = _account_snapshot_from_state(state, email)
        if email == active_email and not (isinstance(entry, dict) and entry.get("last_snapshot")):
            try:
                result = perform_check(home=home, cfg=cfg, notify=False)
                usage = result["usage"]
                snapshot = snapshot_from_response(result["auth"], usage, time.time())
                auth_email = result["auth"].get("email")
                entry = state.get("accounts", {}).get(auth_email) if auth_email else None
            except UsageError as exc:
                entry = {"error": str(exc)}
        status = _account_status(entry, active_email, email)
        snap_5h = "-"
        snap_week = "-"
        snap_reset5 = "-"
        snap_resetw = "-"
        last_seen = "-"
        if isinstance(entry, dict) and entry.get("last_seen"):
            last_seen = str(entry["last_seen"])
        if isinstance(snapshot, dict):
            snap_5h = format_percent(_coerce_float(snapshot.get("used_5h_pct")))
            snap_week = format_percent(_coerce_float(snapshot.get("used_week_pct")))
            snap_reset5 = format_reset(_coerce_float(snapshot.get("reset_5h_sec")))
            snap_resetw = format_reset(_coerce_float(snapshot.get("reset_week_sec")))
        print(f"{email} | {status} | {snap_5h} | {snap_week} | {snap_reset5} | {snap_resetw} | {last_seen}")
    return 0


def cmd_refresh(args: list[str], home: Path) -> int:
    email = ""
    index = 0
    while index < len(args):
        arg = args[index]
        if arg.startswith("--account="):
            email = arg.split("=", 1)[1].strip()
        elif arg in ("--account", "-a"):
            if index + 1 >= len(args):
                print("error: --account requires an email", file=sys.stderr)
                return 2
            email = args[index + 1].strip()
            index += 1
        index += 1
    cfg = load_runtime_config(home)
    credentials = load_credentials(home)
    if not email:
        print("usage: byteask-usage-monitor refresh --account EMAIL", file=sys.stderr)
        return 2
    token = credentials.get(email)
    if not token:
        print(f"error: no stored credential for {email}; run once while that account is active", file=sys.stderr)
        return 1
    auth = load_auth(home)
    if auth.get("email") == email:
        print(f"{email} is the active account; run `status` or `once` instead", file=sys.stderr)
        return 1
    gateway = auth["gateway"]
    try:
        payload = fetch_usage(gateway, token, REQUEST_TIMEOUT_SECONDS)
        usage = parse_usage(payload)
    except UsageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    state = load_state(home)
    _ensure_known_accounts(state, home, cfg, time.time())
    now = time.time()
    _update_account_record(
        state,
        email,
        now=now,
        snapshot=snapshot_from_response(
            {"gateway": gateway, "token": token, "email": email},
            usage,
            now,
        ),
    )
    save_state(state, home)
    print(f"refreshed: {email}")
    print(f"5h: {format_percent(usage.get('used_5h_pct'))}")
    print(f"week: {format_percent(usage.get('used_week_pct'))}")
    return 0


def cmd_refresh_all(args: list[str], home: Path) -> int:
    cfg = load_runtime_config(home)
    try:
        result = perform_tracked_refresh(home, cfg)
    except UsageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    ok = result.get("ok", [])
    errors = result.get("errors", [])
    print(f"refreshed: {len(ok)}")
    for email in ok:
        print(f"  {email}")
    if errors:
        print(f"failed: {len(errors)}")
        for entry in errors:
            print(f"  {entry.get('email')}: {entry.get('error')}")
    return 0 if not errors else 1


def _test_load_auth(tmp: Path) -> None:
    byteask = tmp / ".byteask"
    byteask.mkdir(parents=True, exist_ok=True)
    (byteask / "config.toml").write_text(
        'model_provider = "byteask"\n'
        '\n'
        '[model_providers.byteask]\n'
        'base_url = "https://code.byteask.ai/byteask/v1"\n'
        'experimental_bearer_token = "eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJzdWIiOiAidXNlckBleGFtcGxlLmNvbSIsICJpYXQiOiAxLCJpc3MiOiAiYnl0ZWFzay1nYXRld2F5In0.b2c"\n'
        '\n',
        encoding="utf-8",
    )
    (byteask / "gateway").write_text("https://gateway.example\n", encoding="utf-8")
    auth = load_auth(tmp)
    assert auth["gateway"] == "https://gateway.example"
    assert auth["email"] == "user@example.com"
    assert auth["token"]

    (byteask / "gateway").unlink()
    auth = load_auth(tmp)
    assert auth["gateway"] == "https://code.byteask.ai"

    (byteask / "config.toml").unlink()
    try:
        load_auth(tmp)
    except UsageError:
        pass
    else:
        raise AssertionError("missing config.toml should fail cleanly")

    (byteask / "config.toml").write_text(
        '[model_providers.byteask]\nname = "ByteAsk"\n',
        encoding="utf-8",
    )
    try:
        load_auth(tmp)
    except UsageError:
        pass
    else:
        raise AssertionError("missing token should fail cleanly")


def _test_parse_usage() -> None:
    usage = parse_usage(
        {
            "used_5h_pct": 71.4,
            "used_week_pct": "33",
            "reset_5h_sec": 3661,
            "plan": "free",
            "credit_balance": 12,
        }
    )
    assert usage["used_5h_pct"] == 71.4
    assert usage["used_week_pct"] == 33.0
    assert usage["reset_5h_sec"] == 3661.0
    assert usage["plan"] == "free"

    missing = parse_usage({"used_5h_pct": "42"})
    assert missing["used_week_pct"] is None
    assert missing["reset_5h_sec"] is None
    assert missing["plan"] is None


def _test_notifications(tmp: Path) -> None:
    state: dict[str, Any] = {"notified": {}, "last_snapshot": None, "last_active": None}
    cfg = load_runtime_config(tmp)
    calls: list[tuple[str, int]] = []

    _notify_for_usage(
        {"used_5h_pct": 75.0, "used_week_pct": None},
        state,
        cfg,
        lambda window, threshold: calls.append((window, threshold)),
        now=1000.0,
    )
    assert calls == [("5h", 70)]

    _notify_for_usage(
        {"used_5h_pct": 81.0, "used_week_pct": None},
        state,
        cfg,
        lambda window, threshold: calls.append((window, threshold)),
        now=5000.0,
    )
    assert calls == [("5h", 70), ("5h", 80)]

    _notify_for_usage(
        {"used_5h_pct": 95.0, "used_week_pct": None},
        state,
        cfg,
        lambda window, threshold: calls.append((window, threshold)),
        now=9000.0,
    )
    assert calls == [("5h", 70), ("5h", 80), ("5h", 90)]

    _notify_for_usage(
        {"used_5h_pct": 95.0, "used_week_pct": None},
        state,
        cfg,
        lambda window, threshold: calls.append((window, threshold)),
        now=13000.0,
    )
    assert calls == [("5h", 70), ("5h", 80), ("5h", 90)], "same level must not repeat"

    _notify_for_usage(
        {"used_5h_pct": 85.0, "used_week_pct": None},
        state,
        cfg,
        lambda window, threshold: calls.append((window, threshold)),
        now=17000.0,
    )
    _notify_for_usage(
        {"used_5h_pct": 92.0, "used_week_pct": None},
        state,
        cfg,
        lambda window, threshold: calls.append((window, threshold)),
        now=21000.0,
    )
    assert calls == [("5h", 70), ("5h", 80), ("5h", 90), ("5h", 90)]

    cooldown_cfg = load_runtime_config(tmp)
    cooldown_cfg["cooldown_minutes"] = 10
    cooldown_state: dict[str, Any] = {"notified": {}, "last_snapshot": None, "last_active": 100.0}
    cooldown_calls: list[tuple[str, int]] = []
    _notify_for_usage(
        {"used_5h_pct": 95.0, "used_week_pct": None},
        cooldown_state,
        cooldown_cfg,
        lambda window, threshold: cooldown_calls.append((window, threshold)),
        now=500.0,
    )
    assert cooldown_calls == []
    _notify_for_usage(
        {"used_5h_pct": 95.0, "used_week_pct": None},
        cooldown_state,
        cooldown_cfg,
        lambda window, threshold: cooldown_calls.append((window, threshold)),
        now=1000.0,
    )
    assert cooldown_calls == [("5h", 90)]


def _test_state_permissions(tmp: Path) -> None:
    state: dict[str, Any] = {"notified": {}, "last_snapshot": None, "last_active": None}
    save_state(state, tmp)
    mode = os.stat(state_path(tmp)).st_mode & 0o777
    assert mode == 0o600


def _test_accounts(tmp: Path) -> None:
    cfg = load_runtime_config(tmp)
    state = load_state(tmp)
    _ensure_known_accounts(state, tmp, cfg, 1000.0)
    auth = {"gateway": "https://gateway.example", "token": "abc", "email": "a@example.com"}
    usage = parse_usage({"used_5h_pct": 71.2, "used_week_pct": 35.0, "plan": "free", "reset_5h_sec": 100})
    now = 1100.0
    _update_account_record(
        state,
        "a@example.com",
        now=now,
        snapshot=snapshot_from_response(auth, usage, now),
        token_refreshed_at=now,
    )
    assert state["accounts"]["a@example.com"]["last_snapshot"]["used_5h_pct"] == 71.2
    assert state["accounts"]["a@example.com"]["token_available"] is True
    save_state(state, tmp)
    loaded = load_state(tmp)
    assert "a@example.com" in loaded["accounts"]

    save_credentials({"a@example.com": "abc"}, tmp)
    assert os.stat(credentials_path(tmp)).st_mode & 0o777 == 0o600
    assert load_credentials(tmp)["a@example.com"] == "abc"


def _test_tracked_refresh(tmp: Path) -> None:
    cfg = load_runtime_config(tmp)
    save_credentials({"active@example.com": "aaa", "old@example.com": "bbb"}, tmp)
    state = load_state(tmp)
    _ensure_known_accounts(state, tmp, cfg, 100.0)
    save_state(state, tmp)

    def fake_fetch(gateway: str, token: str, timeout: float) -> dict[str, Any]:
        if token == "bbb":
            return {"used_5h_pct": 12.0, "used_week_pct": 34.0, "plan": "free"}
        raise AssertionError("active token should not be used for tracked refresh")

    # perform_tracked_refresh reads the active account from a temp ~/.byteask, so
    # build it with an active token distinct from the stored old account.
    byteask = tmp / ".byteask"
    byteask.mkdir(exist_ok=True)
    (byteask / "config.toml").write_text(
        '[model_providers.byteask]\n'
        'experimental_bearer_token = "eyJhbGciOiAiSFMyNTYiLCAidHlwIjogIkpXVCJ9.eyJzdWIiOiAiYWN0aXZlQGV4YW1wbGUuY29tIiwgImlhdCI6IDF9.invalid"\n',
        encoding="utf-8",
    )
    (byteask / "gateway").write_text("https://gateway.example\n", encoding="utf-8")

    module = sys.modules[__name__]
    original = module.fetch_usage
    module.fetch_usage = fake_fetch
    try:
        result = perform_tracked_refresh(tmp, cfg)
    finally:
        module.fetch_usage = original
    assert result["ok"] == ["old@example.com"]
    state = load_state(tmp)
    assert state["accounts"]["old@example.com"]["last_snapshot"]["used_5h_pct"] == 12.0


def cmd_selftest(args: list[str], home: Path) -> int:
    import tempfile

    with tempfile.TemporaryDirectory(prefix="byteask-usage-monitor-test-") as raw:
        tmp = Path(raw)
        _test_load_auth(tmp)
        _test_parse_usage()
        _test_notifications(tmp)
        _test_state_permissions(tmp)
        _test_accounts(tmp)
        _test_tracked_refresh(tmp)
    print("self-test: ok")
    return 0


def print_help() -> None:
    print("usage: byteask-usage-monitor <command>")
    print()
    print("commands:")
    print("  status          show current usage from the gateway")
    print("  once            check immediately and notify if a threshold was passed")
    print("  accounts        list known accounts and their last usage snapshots")
    print("  refresh --account EMAIL")
    print("                  refresh a tracked account using its stored credential")
    print("  refresh-all     refresh every tracked account with a stored credential")
    print("  start           start the persistent monitor daemon")
    print("  stop            stop the persistent monitor daemon")
    print("  status-daemon   show whether the daemon is running")
    print("  notify-test     send a test notification")
    print("  self-test       run offline tests")
    print("  help            show this help")


COMMANDS: dict[str, Callable[[list[str], Path], int]] = {
    "status": cmd_status,
    "once": cmd_once,
    "accounts": cmd_accounts,
    "refresh": cmd_refresh,
    "refresh-all": cmd_refresh_all,
    "daemon": cmd_daemon,
    "start": cmd_start,
    "stop": cmd_stop,
    "status-daemon": cmd_status_daemon,
    "notify-test": cmd_notify_test,
    "self-test": cmd_selftest,
}


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] in ("-h", "--help", "help"):
        print_help()
        return 0
    command = argv[1]
    handler = COMMANDS.get(command)
    if handler is None:
        print(f"error: unknown command: {command}", file=sys.stderr)
        print_help()
        return 2
    try:
        return handler(argv[2:], home_dir())
    except UsageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
