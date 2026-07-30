#!/usr/bin/env python3
"""Turn YOLO JSONL into durable, budgeted household activity events.

The bridge is deliberately independent from the video pipeline: it only reads
JSONL from stdin, persists facts to SQLite, and treats LLM narration as an
optional asynchronous enhancement.
"""
import json
import os
import queue
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path


def truthy(value):
    return str(value).lower() not in ("", "0", "false", "no", "off")


@dataclass
class Config:
    enabled: bool
    api_key: str
    api_base: str
    model: str
    min_confidence: float
    event_cooldown: float
    min_request_interval: float
    max_output_tokens: int
    daily_token_budget: int
    retention_days: int
    db_path: str
    replay: bool

    @classmethod
    def from_env(cls, args):
        root = os.environ.get("VISION_DIR", os.getcwd())
        return cls(
            enabled=truthy(os.environ.get("VISION_LLM_ENABLED", "yes")),
            api_key=os.environ.get("LLM_API_KEY", ""),
            api_base=os.environ.get("LLM_API_BASE", "https://api.deepseek.com").rstrip("/"),
            model=os.environ.get("LLM_MODEL", "deepseek-chat"),
            min_confidence=float(os.environ.get("LLM_MIN_CONFIDENCE", "0.4")),
            event_cooldown=float(os.environ.get("LLM_EVENT_COOLDOWN", "10")),
            min_request_interval=float(os.environ.get("LLM_MIN_REQUEST_INTERVAL", "60")),
            max_output_tokens=int(os.environ.get("LLM_MAX_OUTPUT_TOKENS", "120")),
            daily_token_budget=int(os.environ.get("LLM_DAILY_TOKEN_BUDGET", "20000")),
            retention_days=int(os.environ.get("VISION_EVENT_RETENTION_DAYS", "30")),
            db_path=os.environ.get("VISION_EVENT_DB", str(Path(root) / "run" / "vision-events.sqlite3")),
            replay="--replay" in args,
        )


class EventStore:
    def __init__(self, path, retention_days):
        self.path = path
        self.retention_days = retention_days
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self._init()

    def connect(self):
        con = sqlite3.connect(self.path, timeout=5)
        con.row_factory = sqlite3.Row
        con.execute("PRAGMA journal_mode=WAL")
        return con

    def _init(self):
        with self.connect() as con:
            con.executescript("""
              CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY, created_at REAL NOT NULL, kind TEXT NOT NULL,
                class_name TEXT NOT NULL, track_id INTEGER, confidence REAL,
                duration_seconds REAL, facts TEXT NOT NULL, narrative TEXT,
                narrative_source TEXT NOT NULL DEFAULT 'local'
              );
              CREATE TABLE IF NOT EXISTS usage_daily (
                day TEXT PRIMARY KEY, total_tokens INTEGER NOT NULL DEFAULT 0,
                requests INTEGER NOT NULL DEFAULT 0, updated_at REAL NOT NULL
              );
              CREATE INDEX IF NOT EXISTS events_created_at ON events(created_at DESC);
            """)
            con.execute("DELETE FROM events WHERE created_at < ?", (time.time() - self.retention_days * 86400,))

    def add_event(self, event):
        with self.connect() as con:
            cur = con.execute("""INSERT INTO events
                (created_at, kind, class_name, track_id, confidence, duration_seconds, facts, narrative)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
                (event["at"], event["kind"], event["class_name"], event["track_id"],
                 event.get("confidence"), event.get("duration_seconds"),
                 json.dumps(event, ensure_ascii=False), event["local_text"]))
            return cur.lastrowid

    def set_narrative(self, event_id, text):
        with self.connect() as con:
            con.execute("UPDATE events SET narrative=?, narrative_source='llm' WHERE id=?", (text, event_id))

    def tokens_today(self):
        day = time.strftime("%Y-%m-%d")
        with self.connect() as con:
            row = con.execute("SELECT total_tokens FROM usage_daily WHERE day=?", (day,)).fetchone()
            return int(row[0]) if row else 0

    def add_usage(self, tokens):
        day = time.strftime("%Y-%m-%d")
        with self.connect() as con:
            con.execute("""INSERT INTO usage_daily(day,total_tokens,requests,updated_at) VALUES(?,?,1,?)
              ON CONFLICT(day) DO UPDATE SET total_tokens=total_tokens+excluded.total_tokens,
              requests=requests+1, updated_at=excluded.updated_at""", (day, tokens, time.time()))


class Aggregator:
    def __init__(self, cooldown):
        self.cooldown, self.objects = cooldown, {}

    def update(self, detections, now):
        events, active = [], set()
        for item in detections:
            track_id = int(item.get("track_id") or 0)
            if not track_id:
                continue
            active.add(track_id)
            state = self.objects.get(track_id)
            if not state:
                state = {"class_name": item.get("class", "object"), "first_seen": now,
                         "last_seen": now, "movement": "", "announced": False, "last_event": 0,
                         "confidence": item.get("confidence", 0)}
                self.objects[track_id] = state
            state["last_seen"], state["confidence"] = now, item.get("confidence", state["confidence"])
            if not state["announced"] and item.get("tracking_state") == "confirmed":
                state["announced"] = True
                events.append(self.event("appeared", track_id, state, now))
            movement = item.get("movement_state")
            if state["movement"] and movement != state["movement"] and movement in ("moving", "stationary"):
                if now - state["last_event"] >= self.cooldown:
                    events.append(self.event("moving" if movement == "moving" else "stopped", track_id, state, now))
            if movement in ("moving", "stationary"):
                state["movement"] = movement
        for track_id, state in list(self.objects.items()):
            if track_id not in active and now - state["last_seen"] > 5:
                if state["announced"]:
                    events.append(self.event("left", track_id, state, now, now - state["first_seen"]))
                del self.objects[track_id]
        return events

    def event(self, kind, track_id, state, now, duration=None):
        state["last_event"] = now
        names = {"appeared": "刚刚来到画面中", "moving": "开始活动", "stopped": "暂时停留", "left": "已离开画面"}
        cls = state["class_name"]
        text = f"{cls} {names[kind]}"
        if duration:
            text += f"，停留约 {round(duration)} 秒"
        return {"at": now, "kind": kind, "class_name": cls, "track_id": track_id,
                "confidence": state["confidence"], "duration_seconds": duration, "local_text": text}


SYSTEM_PROMPT = """你是家庭视觉日记的文字编辑。根据给定的、可验证的检测事实，用温暖克制的中文写一句不超过50字的观察。不得猜测身份、目的、情绪或风险；不得添加事实中没有的内容。"""


def call_llm(cfg, event):
    url = cfg.api_base + "/v1/chat/completions"
    facts = {key: event.get(key) for key in ("kind", "class_name", "duration_seconds", "confidence")}
    payload = json.dumps({"model": cfg.model, "messages": [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": json.dumps(facts, ensure_ascii=False)}],
        "temperature": 0.65, "max_tokens": cfg.max_output_tokens, "stream": False}).encode("utf-8")
    request = urllib.request.Request(url, data=payload, method="POST",
                                     headers={"Content-Type": "application/json", "Authorization": "Bearer " + cfg.api_key})
    with urllib.request.urlopen(request, timeout=20) as response:
        body = json.loads(response.read().decode("utf-8"))
    return body["choices"][0]["message"]["content"].strip(), int(body.get("usage", {}).get("total_tokens", 0))


class Narrator(threading.Thread):
    def __init__(self, cfg, store):
        super().__init__(daemon=True)
        self.cfg, self.store, self.jobs, self.last_call = cfg, store, queue.Queue(maxsize=100), 0.0

    def submit(self, event_id, event):
        if not self.cfg.enabled or not self.cfg.api_key:
            return
        try:
            self.jobs.put_nowait((event_id, event))
        except queue.Full:
            print("[LLM] narration queue full; keeping local event", file=sys.stderr)

    def run(self):
        while True:
            event_id, event = self.jobs.get()
            # During a busy scene only narrate the most recent settled fact.
            # The earlier facts remain visible in the Dashboard time line.
            while True:
                try:
                    event_id, event = self.jobs.get_nowait()
                except queue.Empty:
                    break
            delay = self.cfg.min_request_interval - (time.time() - self.last_call)
            if delay > 0:
                time.sleep(delay)
            while True:
                try:
                    event_id, event = self.jobs.get_nowait()
                except queue.Empty:
                    break
            if self.store.tokens_today() + self.cfg.max_output_tokens >= self.cfg.daily_token_budget:
                print("[LLM] daily token budget reached; local mode", file=sys.stderr)
                continue
            try:
                text, tokens = call_llm(self.cfg, event)
                self.last_call = time.time()
                self.store.add_usage(tokens)
                self.store.set_narrative(event_id, text)
                print(f"[LLM] event={event_id} tokens={tokens} {text}", flush=True)
            except (urllib.error.URLError, urllib.error.HTTPError, KeyError, ValueError) as exc:
                print(f"[LLM] narration failed: {exc}", file=sys.stderr)


def main():
    cfg = Config.from_env(sys.argv[1:])
    store, aggregator = EventStore(cfg.db_path, cfg.retention_days), Aggregator(cfg.event_cooldown)
    narrator = Narrator(cfg, store)
    narrator.start()
    print(f"[LLM] bridge ready: db={cfg.db_path}, budget={cfg.daily_token_budget}, model={cfg.model}", file=sys.stderr)
    for raw in sys.stdin:
        try:
            frame = json.loads(raw)
        except json.JSONDecodeError:
            continue
        objects = [item for item in frame.get("objects", []) if item.get("confidence", 0) >= cfg.min_confidence]
        for event in aggregator.update(objects, time.time()):
            event_id = store.add_event(event)
            print(f"[EVENT] id={event_id} {event['local_text']}", flush=True)
            narrator.submit(event_id, event)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
