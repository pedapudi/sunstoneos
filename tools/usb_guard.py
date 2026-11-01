"""USB safety for shared board work: churn budget and host health.

On 2026-10-14 the host's PCH xHCI controller (0000:80:14.0) hung
("xHCI host not responding to stop endpoint command", then "HC died") 5.5 s
after a keyboard-mode XIAO disconnected while a tool still had its port open;
every device on that controller was lost, the owner's keyboard, mouse and
Bluetooth adapter included, until a reboot. Earlier boots survived hundreds of
board disconnects, so the risk comes with each abrupt disconnect that finds
transfers in flight. The defect is in the controller; the tools limit how
often they provoke it (docs/developing.md §3.2, "USB safety"):

- Churn budget: every intentional re-enumeration (a flash, RESTART,
  BOOTLOADER, JTAG, a keyboard-mode change) is recorded per board in a state
  file shared by every process (STATE, under /tmp, so one per boot). A new
  one waits until the last one on any board ended SPACING_S ago, and until
  its board has had fewer than BOARD_CAP in the last BOARD_WINDOW_S.
- Health: board work is refused while the kernel log of this boot shows a USB
  host controller that died and was not reset since.
- Placement: a note naming the input devices or Bluetooth adapter a board
  shares its USB controller with (boards work on any port; no advice).
"""

from __future__ import annotations

import contextlib
import fcntl
import glob
import json
import os
import re
import subprocess
import sys
import time

STATE = "/tmp/mimus-usb-churn.json"
STATE_LOCK = "/tmp/mimus-usb-churn.lock"
SYSFS = "/sys"
BOOT_ID = "/proc/sys/kernel/random/boot_id"

# Defaults, each overridable by its environment variable:
# - 15 s between re-enumerations across all boards: the controller's
#   stop-endpoint timeout is 5 s, and a keyboard-mode boot moves the board
#   twice within about 12 s; one event at a time keeps the controller's
#   endpoint work from overlapping.
# - 12 per board per 10 minutes: six flash cycles of a keyboard-mode board
#   (two re-enumerations each). The incident evening had 18 disconnects in
#   its last 8 minutes.
SPACING_S = 15.0
BOARD_CAP = 12
BOARD_WINDOW_S = 600.0
# An event still open after this long (its process crashed) no longer blocks.
MAX_EVENT_S = 300.0
# Events older than this are dropped from the state file.
KEEP_S = 3600.0

FAILED = re.compile(r"xhci_hcd (\S+): (HC died|xHCI host not responding to "
                    r"stop endpoint command)")
REVIVED = re.compile(r"xhci_hcd (\S+): (new USB bus registered|xHCI Host "
                     r"Controller)")
PCI_ADDRESS = re.compile(r"^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$")


def _log(message: str) -> None:
  print(message, file=sys.stderr, flush=True)


def settings() -> tuple[float, int, float]:
  """(spacing s, per-board cap, window s) from the environment."""
  return (float(os.environ.get("MIMUS_USB_SPACING_S", SPACING_S)),
          int(os.environ.get("MIMUS_USB_BOARD_CAP", BOARD_CAP)),
          float(os.environ.get("MIMUS_USB_BOARD_WINDOW_S", BOARD_WINDOW_S)))


# --- Churn budget -----------------------------------------------------------


def _boot_id() -> str:
  try:
    with open(BOOT_ID) as f:
      return f.read().strip()
  except OSError:
    return ""


def _alive(pid: int) -> bool:
  try:
    os.kill(pid, 0)
  except ProcessLookupError:
    return False
  except PermissionError:
    return True
  return True


@contextlib.contextmanager
def _state():
  """The shared state under its lock; saved when the block ends."""
  with open(STATE_LOCK, "a") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    try:
      with open(STATE) as f:
        state = json.load(f)
    except (OSError, ValueError):
      state = {}
    if state.get("boot_id") != _boot_id():
      state = {"boot_id": _boot_id(), "events": []}
    yield state
    tmp = f"{STATE}.{os.getpid()}"
    with open(tmp, "w") as f:
      json.dump(state, f)
    os.replace(tmp, STATE)


def wait_needed(events: list[dict], board: str, count: int, now: float,
                pid: int | None = None) -> tuple[float, str]:
  """Seconds to wait before `board` may re-enumerate `count` times, and why
  (0 and "" when it may go now)."""
  spacing, cap, window = settings()
  pid = os.getpid() if pid is None else pid
  for e in events:
    if (e.get("end") is None and e["pid"] != pid and _alive(e["pid"]) and
        now - e["start"] < MAX_EVENT_S):
      return 1.0, (f"board {e['board']} is re-enumerating ({e['what']}, "
                   f"pid {e['pid']})")
  ends = [e.get("end") or e["start"] for e in events if e["pid"] != pid or
          e.get("end") is not None]
  if ends and now - max(ends) < spacing:
    return spacing - (now - max(ends)), (
        f"{spacing:.0f} s between USB re-enumerations of any board "
        "(MIMUS_USB_SPACING_S)")
  recent = sorted((e for e in events
                   if e["board"] == board and now - e["start"] < window),
                  key=lambda e: e["start"])
  used = sum(e["count"] for e in recent)
  if used + count > cap:
    excess = used + count - cap
    for e in recent:
      excess -= e["count"]
      if excess <= 0:
        return e["start"] + window - now, (
            f"board {board} re-enumerated {used} times in the last "
            f"{window / 60:.0f} min (cap {cap}, MIMUS_USB_BOARD_CAP)")
  return 0.0, ""


@contextlib.contextmanager
def reenumeration(board: str | None, what: str, count: int, log=_log):
  """Holds a place in the churn budget for `count` re-enumerations of
  `board` (its MAC) caused by `what`: waits (printing why) until the budget
  allows them, records the event, and records its end when the block ends.
  count 0 or no board (a UART bridge, which never re-enumerates) passes.
  Yields a dict whose "count" the block may correct to what happened."""
  if not board or count <= 0:
    yield {"count": count}
    return
  told = -1e9
  while True:
    with _state() as state:
      now = time.time()
      events = [e for e in state["events"] if now - e["start"] < KEEP_S]
      wait, why = wait_needed(events, board, count, now)
      if wait <= 0:
        event = {"board": board, "what": what, "count": count, "start": now,
                 "end": None, "pid": os.getpid()}
        events.append(event)
      state["events"] = events
    if wait <= 0:
      break
    if now - told >= 30:
      log(f"usb_guard: {what} on board {board} waits {wait:.0f} s: {why}")
      told = now
    time.sleep(min(max(wait, 0.1), 1.0))
  outcome = {"count": count}
  try:
    yield outcome
  finally:
    with _state() as state:
      for e in state["events"]:
        if (e["pid"] == event["pid"] and e["start"] == event["start"] and
            e["board"] == board):
          e["end"] = time.time()
          e["count"] = max(0, int(outcome["count"]))


def expected_reenumerations(command: str, mode: str) -> int:
  """How many times a command re-enumerates an S3 board's USB connection.
  mode: the console's port now ("cdc" or "jtag"). The estimate assumes
  keyboard mode is enabled, so a flash or RESTART costs 2 from keyboard mode
  (off the CDC port, then the new boot's switch back to it) and 1 from
  USB-Serial-JTAG; a flash corrects it afterwards to what happened
  (board.flash_and_verify)."""
  words = command.split()
  word = words[0].upper() if words else ""
  leave = 1 if mode == "cdc" else 0  # the CDC device goes away
  if word == "RESTART" or word == "FLASH":
    return leave + 1  # and the new boot switches back
  if word in ("BOOTLOADER", "JTAG"):
    return leave
  return 0


# --- Host health ------------------------------------------------------------


def kernel_log() -> str | None:
  """This boot's kernel log, or None if it can't be read."""
  try:
    proc = subprocess.run(["journalctl", "-k", "-b", "--no-pager", "-q",
                           "-o", "short-precise"], capture_output=True,
                          text=True, timeout=30)
  except (OSError, subprocess.TimeoutExpired):
    return None
  return proc.stdout if proc.returncode == 0 else None


def failed_controllers(log_text: str) -> dict[str, str]:
  """USB host controllers (PCI address) whose last event in the log is a
  failure, with that log line. A controller registered again later (rebound
  by hand or by tools/host/xhci-watchdog) counts as recovered."""
  failed: dict[str, str] = {}
  for line in log_text.splitlines():
    if m := FAILED.search(line):
      failed[m[1]] = line.strip()
    elif m := REVIVED.search(line):
      failed.pop(m[1], None)
  return failed


def recovery_steps(pci: str, line: str) -> str:
  return f"""usb_guard: USB host controller {pci} failed during this boot:
  {line}
Every device on it (boards, and any keyboard, mouse or Bluetooth adapter it
serves) stays disconnected until the controller is reset, so board work is
refused. Recovery, by the owner:
  1. Save work and reboot; or reset just that controller (needs sudo):
       echo {pci} | sudo tee /sys/bus/pci/drivers/xhci_hcd/unbind
       sleep 2; echo {pci} | sudo tee /sys/bus/pci/drivers/xhci_hcd/bind
     (tools/host/README.md describes an optional watchdog that does this).
  2. Check that the boards are back: ls -l /dev/serial/by-id/
MIMUS_USB_HEALTH=warn proceeds anyway."""


_health_checked = 0.0


def check_health(log=_log, read_log=None) -> None:
  """Refuses board work (SystemExit) while a USB host controller that died
  this boot has not been reset. Checked at most once a minute per process."""
  global _health_checked
  if time.monotonic() - _health_checked < 60:
    return
  text = (read_log or kernel_log)()
  if text is None:
    log("usb_guard: warning: cannot read the kernel log (journalctl -k -b); "
        "USB host health not checked")
    return
  failed = failed_controllers(text)
  if failed:
    message = "\n".join(recovery_steps(pci, line)
                        for pci, line in failed.items())
    if os.environ.get("MIMUS_USB_HEALTH") == "warn":
      log(message + "\nusb_guard: MIMUS_USB_HEALTH=warn: continuing")
    else:
      raise SystemExit(message)
  _health_checked = time.monotonic()


def pci_controller(sysfs_path: str) -> str | None:
  """The PCI address of the USB host controller above a sysfs device."""
  parts = os.path.realpath(sysfs_path).split("/")
  for i, part in enumerate(parts):
    if re.fullmatch(r"usb\d+", part) and i > 0 and PCI_ADDRESS.match(parts[i - 1]):
      return parts[i - 1]
  return None


def tty_controller(port: str) -> str | None:
  tty = os.path.basename(os.path.realpath(port))
  return pci_controller(os.path.join(SYSFS, "class", "tty", tty, "device"))


def neighbours(controller: str) -> list[str]:
  """Input devices (not mimus boards) and Bluetooth adapters on a USB host
  controller."""
  found = []
  for entry in sorted(glob.glob(os.path.join(SYSFS, "class", "input", "input*"))):
    device = os.path.realpath(os.path.join(entry, "device"))
    if ":303A:" in device.upper() or pci_controller(device) != controller:
      continue
    try:
      with open(os.path.join(entry, "name")) as f:
        name = f.read().strip()
    except OSError:
      name = os.path.basename(entry)
    if name not in found:
      found.append(name)
  for entry in sorted(glob.glob(os.path.join(SYSFS, "class", "bluetooth", "hci*"))):
    if not re.fullmatch(r"hci\d+", os.path.basename(entry)):
      continue  # hci0:50 and the like are connections, not adapters
    if pci_controller(os.path.join(entry, "device")) == controller:
      found.append(f"Bluetooth {os.path.basename(entry)}")
  return found


def placement_warning(port: str) -> str | None:
  """A note if the board on `port` shares its USB host controller with the
  host's input devices or Bluetooth adapter, else None: a failure of that
  controller takes them with it. Boards work on any port; this states the
  shared controller and recommends none."""
  controller = tty_controller(port)
  if controller is None:
    return None
  shared = neighbours(controller)
  if not shared:
    return None
  return (f"usb_guard: note: {port} is on USB controller {controller} with "
          f"{', '.join(shared)}; a failure of that controller also "
          "disconnects them.")


_warned: set[str] = set()


def warn_placement(port: str, log=_log) -> None:
  """warn once per process and controller."""
  try:
    message = placement_warning(port)
  except OSError:
    return
  controller = tty_controller(port)
  if message and controller not in _warned:
    _warned.add(controller)
    log(message)
