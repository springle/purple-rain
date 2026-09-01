#!/usr/bin/env bash
# Timer entry point (every 5 min): build the Purple Rain payload from live
# sources and PUT it to the Cloudflare Worker. KV is the single source of
# truth; there is no git in the path.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
REPO="$HOME/Developer/purple-rain"
OUT="$HOME/.local/state/purple-rain/purple.json"
URL="https://pebble-wx.sam-2d3.workers.dev/wx/purple.json"
ENV_FILE="$HOME/.config/pebble-wx/cloudflare.env"  # PUBLISH_TOKEN=...
mkdir -p "$(dirname "$OUT")"

cd "$REPO/pipeline"
uv run --reinstall-package purple-rain-pipeline purple-rain "$OUT"

# shellcheck disable=SC1090
source "$ENV_FILE"
# receipts: keep 48h of published payloads for reconciliation
HIST="$HOME/.local/state/purple-rain/history"
mkdir -p "$HIST"
cp "$OUT" "$HIST/purple-$(date -u +%Y%m%d-%H%M).json"
find "$HIST" -name 'purple-*.json' -mmin +2880 -delete

for attempt in 1 2 3; do
  if curl -sf -X PUT "$URL" \
      -H "authorization: Bearer $PUBLISH_TOKEN" \
      --data-binary @"$OUT" >/dev/null; then
    echo "published $(wc -c <"$OUT") bytes"
    exit 0
  fi
  echo "publish attempt $attempt failed; retrying" >&2
  sleep 10
done
echo "publish failed after 3 attempts" >&2
exit 1
