#!/usr/bin/env python3
"""Small dependency-free dashboard for the household activity event store."""
import argparse
import json
import os
import sqlite3
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


PAGE = r'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>家里的此刻</title><style>
:root{--ink:#23312d;--muted:#718078;--paper:#f7f5ef;--card:#fffdf8;--line:#e7e2d6;--accent:#4f8272}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px system-ui,"Microsoft YaHei",sans-serif}.wrap{max-width:1200px;margin:auto;padding:24px}header{display:flex;justify-content:space-between;align-items:center;margin-bottom:18px}h1{font-size:25px;margin:0}.sub{color:var(--muted);font-size:14px}.grid{display:grid;grid-template-columns:1.45fr 1fr;gap:18px}.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:18px;box-shadow:0 8px 25px #6b705c0c}.video{min-height:390px;padding:0;overflow:hidden;background:#1e2723}.video iframe{border:0;width:100%;height:100%;min-height:390px}.title{font-size:14px;letter-spacing:.05em;color:var(--muted);margin-bottom:12px}.now{font-size:22px;line-height:1.45;margin:2px 0 12px}.metrics{display:flex;gap:14px;font-size:13px;color:var(--muted);flex-wrap:wrap}.timeline{margin-top:18px}.event{padding:14px 0;border-top:1px solid var(--line)}.event:first-child{border-top:0}.event time{font-size:12px;color:var(--muted);float:right}.event p{margin:5px 0;line-height:1.45}.facts{font-size:12px;color:var(--muted)}.empty{color:var(--muted);padding:28px 0}@media(max-width:760px){.wrap{padding:14px}.grid{grid-template-columns:1fr}.video,.video iframe{min-height:240px}.now{font-size:19px}}
</style><body><main class="wrap"><header><div><h1>家里的此刻</h1><div class="sub">实时画面与温和的活动记录</div></div><div id="status" class="sub">正在连接…</div></header><section class="grid"><div class="card video"><iframe id="video" title="实时画面"></iframe></div><div class="card"><div class="title">最近动态</div><div id="now" class="now">等待画面中的活动…</div><div id="metrics" class="metrics"></div></div></section><section class="card timeline"><div class="title">今天的生活动态</div><div id="events" class="empty">暂无已记录的活动</div></section></main><script>
const esc=s=>String(s??'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
function stamp(t){return new Date(t*1000).toLocaleTimeString('zh-CN',{hour:'2-digit',minute:'2-digit'})}
async function refresh(){try{const r=await fetch('/api/overview',{cache:'no-store'});const d=await r.json();document.querySelector('#status').textContent=d.status;document.querySelector('#metrics').innerHTML=`今日 AI 用量 ${d.usage.total_tokens}/${d.usage.budget} tokens · ${d.usage.requests} 次`;const e=d.events||[];document.querySelector('#now').textContent=e[0]?.narrative||'今天还没有可记录的活动。';document.querySelector('#events').className='';document.querySelector('#events').innerHTML=e.length?e.map(x=>`<article class="event"><time>${stamp(x.created_at)}</time><p>${esc(x.narrative||x.local_text)}</p><div class="facts">事实：${esc(x.class_name)} · ${esc(x.kind)} · 轨迹 #${esc(x.track_id??'-')}${x.duration_seconds?` · 约 ${Math.round(x.duration_seconds)} 秒`:''}</div></article>`).join(''):'<div class="empty">暂无已记录的活动</div>'}catch(e){document.querySelector('#status').textContent='暂时无法读取活动记录'}}
document.querySelector('#video').src=location.protocol+'//'+location.hostname+':8889/vision';refresh();setInterval(refresh,4000);
</script></body></html>'''


class App(BaseHTTPRequestHandler):
    db_path = ""
    budget = 20000
    def log_message(self, *_): pass
    def send_json(self, value):
        body = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if urlparse(self.path).path == "/api/overview":
            events, tokens, requests = [], 0, 0
            if Path(self.db_path).exists():
                con = sqlite3.connect(self.db_path); con.row_factory = sqlite3.Row
                try:
                    events = [dict(row) for row in con.execute("SELECT id,created_at,kind,class_name,track_id,duration_seconds,facts,narrative,narrative_source FROM events WHERE created_at>=? ORDER BY id DESC LIMIT 100", (time.time()-86400,))]
                    for row in events:
                        facts = json.loads(row.pop("facts")); row["local_text"] = facts.get("local_text", "")
                    usage = con.execute("SELECT total_tokens,requests FROM usage_daily WHERE day=?", (time.strftime("%Y-%m-%d"),)).fetchone()
                    if usage: tokens, requests = usage
                finally: con.close()
            self.send_json({"status":"实时更新中" if Path(self.db_path).exists() else "等待 YOLO 事件库", "events":events, "usage":{"total_tokens":tokens,"requests":requests,"budget":self.budget}}); return
        body = PAGE.encode(); self.send_response(200); self.send_header("Content-Type", "text/html; charset=utf-8"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)


def main():
    p = argparse.ArgumentParser(); p.add_argument("--db", required=True); p.add_argument("--host", default="0.0.0.0"); p.add_argument("--port", type=int, default=8080); p.add_argument("--budget", type=int, default=20000); args=p.parse_args()
    App.db_path, App.budget = args.db, args.budget
    print(f"[DASHBOARD] http://{args.host}:{args.port}", flush=True)
    ThreadingHTTPServer((args.host, args.port), App).serve_forever()

if __name__ == "__main__": main()
