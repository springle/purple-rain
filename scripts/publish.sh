#!/usr/bin/env bash
# Timer entry point (every 2 min — MRMS's native cadence): build the Purple
# Rain payload from live sources and PUT it to the Cloudflare Worker. KV is
# the single source of truth; there is no git in the path.
#
# Publish-on-change: an unchanged payload (ignoring gen) is skipped, except
# that gen is refreshed at least every 10 min so the watch's staleness
# disclosure (red dot after 20 min) never fires on a quiet day.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
REPO="$HOME/Developer/purple-rain"
STATE="$HOME/.local/state/purple-rain"
OUT="$STATE/purple.json"
LAST="$STATE/last-published.json"
HIST="$STATE/history"
URL="https://pebble-wx.sam-2d3.workers.dev/wx/purple.json"
ENV_FILE="$HOME/.config/pebble-wx/cloudflare.env"  # PUBLISH_TOKEN=...
mkdir -p "$STATE" "$HIST"

cd "$REPO/pipeline"
uv run --reinstall-package purple-rain-pipeline purple-rain "$OUT"

if [ -f "$LAST" ] && python3 - "$OUT" "$LAST" <<'PY'
import json, sys
a = json.load(open(sys.argv[1])); b = json.load(open(sys.argv[2]))
a.pop("gen", None); b.pop("gen", None)
sys.exit(0 if a == b else 1)
PY
then
  age=$(( $(date +%s) - $(stat -c %Y "$LAST") ))
  if [ "$age" -lt 600 ]; then
    echo "unchanged; publish skipped"
    exit 0
  fi
fi

# shellcheck disable=SC1090
source "$ENV_FILE"
for attempt in 1 2 3; do
  if curl -sf -X PUT "$URL" \
      -H "authorization: Bearer $PUBLISH_TOKEN" \
      --data-binary @"$OUT" >/dev/null; then
    cp "$OUT" "$LAST"
    cp "$OUT" "$HIST/purple-$(date -u +%Y%m%d-%H%M).json"
    find "$HIST" -name 'purple-*.json' -mmin +2880 -delete
    echo "published $(wc -c <"$OUT") bytes"
    exit 0
  fi
  echo "publish attempt $attempt failed; retrying" >&2
  sleep 10
done
echo "publish failed after 3 attempts" >&2
exit 1
