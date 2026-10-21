#!/usr/bin/env python3
"""
Render images/demo-json.png and images/demo-protobuf.png from real query output.

Needs the demo data (`make generate-data`), a release build and Chrome or Chromium. Run from the repository root:
    python3 scripts/generate-demo-screenshots.py
"""

import html
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DUCKDB = ROOT / "build/release/duckdb"
EXTENSION = ROOT / "build/release/extension/nats_js/nats_js.duckdb_extension"
OUT_DIR = ROOT / "build/demo"

DEMOS = [
    {
        "image": "demo-json.png",
        "subtitle": "JSON payloads: subject wildcards and field extraction",
        "sql": """SELECT seq, subject, device_id, kw, voltage
FROM nats_scan('telemetry',
    subject := 'telemetry.dc1.power.>',
    json_extract := ['device_id', 'kw', 'voltage']
)
LIMIT 5;""",
    },
    {
        "image": "demo-protobuf.png",
        "subtitle": "Protocol Buffers: nested fields with native types",
        "sql": """SELECT location_zone,
       count(*) AS readings,
       round(avg(metrics_kw), 2) AS avg_kw,
       max(metrics_voltage) AS peak_voltage
FROM nats_scan('telemetry_proto',
    proto_file := 'test/proto/telemetry.proto',
    proto_message := 'Telemetry',
    proto_extract := ['location.zone', 'metrics.kw', 'metrics.voltage']
)
GROUP BY location_zone
ORDER BY location_zone;""",
    },
]

KEYWORDS = r"\b(SELECT|FROM|WHERE|GROUP BY|ORDER BY|LIMIT|AS|AND|INSTALL|LOAD)\b"

PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<style>
  * {{ margin: 0; padding: 0; box-sizing: border-box; }}
  body {{ font-family: Menlo, Consolas, Monaco, monospace; background: #1e1e1e; padding: 0; }}
  .header {{ background: #2d2d30; padding: 20px 30px; border-bottom: 1px solid #3e3e42; }}
  .header h1 {{ color: #ffd43b; font-size: 24px; font-weight: 600; margin-bottom: 8px; }}
  .header p {{ color: #a0a0a0; font-size: 14px; }}
  .section {{ padding: 30px; }}
  .terminal {{ background: #0e0e0e; border: 1px solid #3e3e42; border-radius: 6px; padding: 20px; }}
  pre {{ font-family: inherit; font-size: 14px; line-height: 1.45; color: #d4d4d4; }}
  .prompt {{ color: #4ec9b0; }}
  .kw {{ color: #569cd6; font-weight: 600; }}
  .str {{ color: #ce9178; }}
  .fn {{ color: #dcdcaa; }}
  .result {{ margin-top: 18px; line-height: 1.15; }}
</style>
</head>
<body>
  <div class="header">
    <h1>DuckDB NATS JetStream Extension</h1>
    <p>{subtitle}</p>
  </div>
  <div class="section">
    <div class="terminal">
      <pre>{sql}</pre>
      <pre class="result">{result}</pre>
    </div>
  </div>
</body>
</html>
"""


def highlight(sql):
    parts = re.split(r"('[^']*')", sql)
    out = []
    for part in parts:
        if part.startswith("'"):
            out.append(f'<span class="str">{html.escape(part)}</span>')
        else:
            text = html.escape(part)
            text = re.sub(KEYWORDS, r'<span class="kw">\1</span>', text)
            text = text.replace("nats_scan", '<span class="fn">nats_scan</span>')
            out.append(text)
    lines = "".join(out).split("\n")
    return "\n".join(('<span class="prompt">D </span>' if i == 0 else "  ") + line for i, line in enumerate(lines))


def run_query(sql):
    script = f"LOAD '{EXTENSION}';\n{sql}\n"
    result = subprocess.run([str(DUCKDB), "-no-agent", "-unsigned"], input=script, capture_output=True,
                            text=True, cwd=ROOT, check=True)
    if "Error" in result.stdout or result.stderr.strip():
        sys.exit(f"query failed:\n{result.stdout}{result.stderr}")
    return result.stdout.rstrip("\n")


def find_chrome():
    for name in ("google-chrome", "chromium", "chromium-browser"):
        if shutil.which(name):
            return shutil.which(name)
    mac = Path("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
    if mac.exists():
        return str(mac)
    sys.exit("Chrome or Chromium not found")


def main():
    chrome = find_chrome()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for demo in DEMOS:
        result = run_query(demo["sql"])
        page = PAGE.format(subtitle=html.escape(demo["subtitle"]), sql=highlight(demo["sql"]),
                           result=html.escape(result))
        page_path = OUT_DIR / demo["image"].replace(".png", ".html")
        page_path.write_text(page, encoding="utf-8")
        height = 230 + 21 * demo["sql"].count("\n") + 17 * result.count("\n")
        image = ROOT / "images" / demo["image"]
        subprocess.run([chrome, "--headless", "--hide-scrollbars", f"--screenshot={image}",
                        f"--window-size=1100,{height}", page_path.as_uri()], check=True, capture_output=True)
        print(f"wrote {image.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
