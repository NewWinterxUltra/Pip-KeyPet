"""Read-only probe of the Windows notification database (for Pip Link).

Prints the schema and a few recent payloads, so the parser can be built against
the real Windows 10/11 format instead of a guess. Changes nothing.
"""
import os
import shutil
import sqlite3
import tempfile
import xml.etree.ElementTree as ET

SRC = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "Windows", "Notifications", "wpndatabase.db")


def connect():
    uri = "file:" + SRC.replace("\\", "/") + "?immutable=1"
    try:
        con = sqlite3.connect(uri, uri=True, timeout=2)
        con.execute("select count(*) from Notification").fetchone()
        return con
    except Exception:
        tmp = os.path.join(tempfile.gettempdir(), "pip-wpndatabase.db")
        shutil.copy2(SRC, tmp)
        return sqlite3.connect(tmp, timeout=2)


def main():
    print("db:", SRC, "exists:", os.path.exists(SRC))
    if not os.path.exists(SRC):
        return
    con = connect()
    cur = con.cursor()
    tables = [r[0] for r in cur.execute("select name from sqlite_master where type='table'")]
    print("tables:", tables)
    for t in ("Notification", "NotificationHandler"):
        try:
            cols = [r[1] for r in cur.execute("pragma table_info(%s)" % t)]
            n = cur.execute("select count(*) from %s" % t).fetchone()[0]
            print(t, "cols:", cols, "rows:", n)
        except Exception as e:
            print(t, "ERR", e)
    try:
        rows = cur.execute("select Id, Payload, ArrivalTime from Notification order by Id desc limit 3").fetchall()
        for r in rows:
            print("ID", r[0], "ARR", r[2])
            print("PAYLOAD", repr(r[1][:1200]))
            try:
                root = ET.fromstring(r[1])
                print("TEXTS", [e.text for e in root.iter() if e.tag.endswith("text") and e.text])
            except Exception as e:
                print("XML ERR", e)
    except Exception as e:
        print("read err", e)
    try:
        rows = cur.execute("select * from NotificationHandler limit 10").fetchall()
        print("handlers:", rows)
    except Exception as e:
        print("handler err", e)
    con.close()


if __name__ == "__main__":
    main()
