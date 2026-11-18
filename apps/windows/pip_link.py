"""Pip Link for Windows: notifications, music and time for the Pip keychain.

The phone link in the firmware is a small BLE service (docs/companion.md). iPhone uses Apple's ANCS; Android and
Windows have no such system service, so this program reads what Windows already has and passes it over Bluetooth.

Run:  python pip_link.py            (console window with the log)
      python pip_link.py --dump     (only show what it would send; no Bluetooth)

Requires: pip install bleak
Optional: pip install winsdk        (track title/artist; media keys work without it)
"""
from __future__ import annotations

import argparse
import asyncio
import ctypes
import datetime as dt
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

APP = "Pip Link"
VERSION = "1.0"
SERVICE = "50495044-4247-4b45-5950-455400000010"
RX = "50495044-4247-4b45-5950-455400000011"
TX = "50495044-4247-4b45-5950-455400000012"
DEVICE_NAME = "Pip"

DB = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "Windows", "Notifications", "wpndatabase.db")

try:
    from bleak import BleakClient, BleakScanner
except Exception as e:  # pragma: no cover - only on a machine without bleak
    BleakClient = BleakScanner = None
    BLE_IMPORT_ERROR = e
else:
    BLE_IMPORT_ERROR = None

try:
    from winsdk.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as MediaManager
except Exception:
    MediaManager = None


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def tz_minutes() -> int:
    """The current UTC offset in minutes east, DST included."""
    try:
        return int(time.localtime().tm_gmtoff / 60)
    except Exception:
        try:
            return int(-(time.altzone if time.localtime().tm_isdst and time.daylight else time.timezone) / 60)
        except Exception:
            return int(-time.timezone / 60)


def clean(s, n):
    """One protocol field: no pipe, no newlines, at most n chars."""
    s = (s or "").replace("|", "/").replace("\r", " ").replace("\n", " ")
    return " ".join(s.split())[:n]


# ---------------------------------------------------------------- notifications

APPS = [
    ("chrome", "Chrome"), ("msedge", "Edge"), ("firefox", "Firefox"), ("brave", "Brave"), ("opera", "Opera"),
    ("telegram", "Telegram"), ("whatsapp", "WhatsApp"), ("discord", "Discord"), ("slack", "Slack"),
    ("spotify", "Spotify"), ("teams", "Teams"), ("outlook", "Outlook"), ("thunderbird", "Почта"),
    ("yourphone", "Phone Link"), ("phoneexperiencehost", "Phone Link"), ("windows.defender", "Defender"),
    ("immersivecontrolpanel", "Параметры"), ("windowsstore", "Store"), ("explorer", "Проводник"),
    ("shellexperiencehost", "Windows"), ("contentdeliverymanager", "Windows"), ("systemsettings", "Параметры"),
    ("viber", "Viber"), ("skype", "Skype"), ("zoom", "Zoom"), ("steam", "Steam"), ("epicgames", "Epic Games"),
]

SYSTEM_NOISE = ("windows.defender", "contentdeliverymanager", "shellexperiencehost", "windowsstore")


def app_name(aumid: str) -> str:
    low = (aumid or "").lower()
    for key, name in APPS:
        if key in low:
            return name
    if not aumid:
        return "Windows"
    part = re.split(r"[._!]", aumid.split("_")[0])[0]
    return part[:23] or "Windows"


def filetime(value) -> dt.datetime:
    try:
        return dt.datetime(1601, 1, 1) + dt.timedelta(microseconds=int(value) / 10)
    except Exception:
        return dt.datetime.now()


def parse_payload(payload: bytes):
    """(title, text) from a toast XML; attribution lines and empty text are dropped."""
    try:
        root = ET.fromstring(payload.decode("utf-8", "replace"))
    except Exception:
        return "", ""
    texts = []
    for e in root.iter():
        if not e.tag.endswith("text"):
            continue
        if e.get("placement") == "attribution":
            continue
        t = " ".join((e.text or "").split())
        if t:
            texts.append(t)
    if not texts:
        for e in root.iter():
            if e.tag.endswith("header") and e.get("title"):
                texts.append(" ".join(e.get("title").split()))
                break
    title = texts[0] if texts else ""
    body = texts[1] if len(texts) > 1 else ""
    return title, body


class Notifications:
    def __init__(self):
        self.known: set[int] = set()
        self.sent: set[int] = set()
        self.last_id = 0

    def _connect(self):
        uri = "file:" + DB.replace("\\", "/") + "?immutable=1"
        try:
            con = sqlite3.connect(uri, uri=True, timeout=2)
            con.execute("select count(*) from Notification").fetchone()
            return con
        except Exception:
            tmp = os.path.join(tempfile.gettempdir(), "pip-wpndatabase.db")
            shutil.copy2(DB, tmp)
            return sqlite3.connect(tmp, timeout=2)

    def fetch(self):
        """id -> (app, title, text, hour, minute), newest first."""
        out = {}
        if not os.path.exists(DB):
            return out
        try:
            con = self._connect()
            rows = con.execute(
                "select n.Id, n.Payload, n.ArrivalTime, h.PrimaryId "
                "from Notification n left join NotificationHandler h on h.RecordId = n.HandlerId"
            ).fetchall()
            con.close()
        except Exception as e:
            log("notifications: cannot read the database:", e)
            return out
        for nid, payload, arrival, primary in rows:
            title, body = parse_payload(payload or b"")
            if not title and not body:
                continue
            when = filetime(arrival)
            out[int(nid)] = (app_name(primary or ""), title, body, when.hour, when.minute)
        return out

    def prime(self):
        """Remember what is already in the Action Center, without sending it."""
        now = self.fetch()
        self.known = set(now)
        self.sent.clear()
        self.last_id = max(now) if now else 0

    def poll(self):
        """(new [(uid, app, title, text, hh, mm)], gone [uid])."""
        now = self.fetch()
        new = [(uid, *now[uid]) for uid in now if uid not in self.known]
        gone = [uid for uid in self.known if uid not in now and uid in self.sent]
        self.known = set(now)
        for item in new:
            self.sent.add(item[0])
        return new, gone


# ---------------------------------------------------------------- media

def media_key(vk):
    """A global media/volume key: works with any player, no COM needed."""
    try:
        ctypes.windll.user32.keybd_event(vk, 0, 0, 0)
        ctypes.windll.user32.keybd_event(vk, 0, 2, 0)
    except Exception as e:
        log("media key failed:", e)


VK = {"play_pause": 0xB3, "next": 0xB0, "prev": 0xB1, "vol_up": 0xAF, "vol_down": 0xAE}


class Media:
    def __init__(self):
        self.last = None

    async def track(self):
        """(title, artist, dur, pos, playing) or None."""
        if MediaManager is None:
            return None
        try:
            mgr = await MediaManager.request_async()
            sess = mgr.get_current_session()
            if not sess:
                return None
            props = await sess.try_get_media_properties_async()
            info = sess.get_playback_info()
            timeline = sess.get_timeline_properties()
            playing = int(info.playback_status) == 4
            pos = timeline.position.total_seconds() if timeline else 0
            dur = timeline.end_time.total_seconds() if timeline else 0
            return (props.title or "", props.artist or "", int(dur), pos, playing)
        except Exception:
            return None

    async def command(self, what):
        if MediaManager is not None:
            try:
                mgr = await MediaManager.request_async()
                sess = mgr.get_current_session()
                if sess:
                    if what == "play_pause":
                        await sess.try_play_pause_async()
                        return
                    if what == "next":
                        await sess.try_skip_next_async()
                        return
                    if what == "prev":
                        await sess.try_skip_previous_async()
                        return
            except Exception:
                pass
        key = {"play_pause": VK["play_pause"], "next": VK["next"], "prev": VK["prev"],
               "vol_up": VK["vol_up"], "vol_down": VK["vol_down"]}.get(what)
        if key:
            media_key(key)


# ---------------------------------------------------------------- BLE

class Link:
    def __init__(self, name=DEVICE_NAME, dump=False):
        self.name = name
        self.dump = dump
        self.client = None
        self.notes = Notifications()
        self.media = Media()
        self.privacy = 1
        self.buf = ""
        self.tx = []

    # -- low level
    async def send(self, line):
        if self.dump:
            print("→", line)
            return
        if not self.client or not self.client.is_connected:
            return
        try:
            await self.client.write_gatt_char(RX, (line + "\n").encode("utf-8"), response=False)
        except Exception as e:
            log("write failed:", e)

    def on_tx(self, _, data: bytearray):
        self.buf += data.decode("utf-8", "replace")
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            self.tx.append(line.strip("\r"))

    async def handle(self):
        while self.tx:
            line = self.tx.pop(0)
            log("←", line)
            if line.startswith("PRIVACY "):
                try:
                    self.privacy = int(line.split()[1])
                except Exception:
                    pass
            elif line.startswith("CMD "):
                await self.command(line[4:])
            elif line.startswith("STATE "):
                pass

    async def command(self, text):
        parts = text.split()
        if not parts:
            return
        cmd = parts[0]
        if cmd == "DISMISS":
            # Windows does not let an unpackaged app remove another app's toast; Pip still clears it locally.
            log("dismiss requested for", parts[1] if len(parts) > 1 else "?")
        elif cmd in ("PLAY_PAUSE", "NEXT", "PREV", "VOL_UP", "VOL_DOWN"):
            await self.media.command(cmd.lower())
        elif cmd in ("ANSWER", "DECLINE"):
            log("calls are not available on Windows")

    async def find(self):
        log("scanning for", self.name, "...")
        found = await BleakScanner.discover(timeout=6.0, return_adv=True)
        for dev, adv in found.values():
            if (dev.name or adv.local_name or "") == self.name:
                return dev
        return None

    async def run_once(self):
        dev = await self.find()
        if not dev:
            log("Pip not found (is it awake and in range?)")
            return
        log("connecting to", dev.address)
        async with BleakClient(dev, timeout=15) as client:
            self.client = client
            log("connected")
            await client.start_notify(TX, self.on_tx)
            await self.send("HELLO windows " + VERSION)
            await self.send(f"TIME {int(time.time())} {tz_minutes()}")
            self.notes.prime()
            last_time = last_ping = time.monotonic()
            last_sig = None
            last_track_at = 0.0
            while client.is_connected:
                await self.handle()
                new, gone = self.notes.poll()
                if self.privacy != 2:
                    for uid, app, title, body, hh, mm in new:
                        await self.send(f"NOTE {uid}|{clean(app, 23)}|{clean(title, 47)}|{clean(body, 460)}|{hh}|{mm}|0")
                    for uid in gone:
                        await self.send(f"NOTE_GONE {uid}")
                tr = await self.media.track()
                now = time.monotonic()
                sig = None if not tr else (tr[0], tr[1], tr[2], tr[4])
                if sig != last_sig or (tr is not None and now - last_track_at > 5):
                    last_sig, last_track_at = sig, now
                    if tr:
                        title, artist, dur, pos, playing = tr
                        await self.send(f"TRACK {clean(title, 63)}|{clean(artist, 47)}|{dur}|{pos:.1f}|{1 if playing else 0}")
                    else:
                        await self.send("TRACK ||0|0|0")
                if now - last_time > 60:
                    last_time = now
                    await self.send(f"TIME {int(time.time())} {tz_minutes()}")
                if now - last_ping > 30:
                    last_ping = now
                    await self.send("PING")
                await asyncio.sleep(1.0)
        self.client = None
        log("disconnected")

    async def run(self):
        if BleakClient is None:
            log("bleak is not installed:", BLE_IMPORT_ERROR)
            log("run: pip install bleak")
            return
        while True:
            try:
                await self.run_once()
            except Exception as e:
                log("link:", e)
            await asyncio.sleep(5)


# ---------------------------------------------------------------- main

def dump_once():
    n = Notifications()
    n.prime()
    log("already in the Action Center:", len(n.known))
    log("waiting for new notifications... (Ctrl+C to stop)")
    while True:
        new, gone = n.poll()
        for uid, app, title, body, hh, mm in new:
            print(f"NOTE {uid}|{clean(app, 23)}|{clean(title, 47)}|{clean(body, 460)}|{hh}|{mm}|0")
        for uid in gone:
            print(f"NOTE_GONE {uid}")
        time.sleep(1)


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description=APP)
    ap.add_argument("--dump", action="store_true", help="print notifications instead of using Bluetooth")
    ap.add_argument("--name", default=DEVICE_NAME, help="Bluetooth name of the keychain")
    a = ap.parse_args()
    log(f"{APP} {VERSION}")
    if a.dump:
        try:
            dump_once()
        except KeyboardInterrupt:
            pass
        return
    if os.name != "nt":
        log("this app is for Windows")
        return
    try:
        asyncio.run(Link(a.name).run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
