/* Purple Rain companion: pull the payload from Cloudflare every 5 minutes,
 * push to the watch only when it changed. */
var URL = 'https://pebble-wx.sam-2d3.workers.dev/wx/purple.json';
var POLL_MS = 2 * 60 * 1000;

function fetchPayload() {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', URL, true);
  xhr.timeout = 20000;
  xhr.onload = function () {
    if (xhr.status !== 200) return;
    try { deliver(JSON.parse(xhr.responseText)); }
    catch (e) { console.log('purple: bad payload ' + e); }
  };
  xhr.send();
}

var sentThisSession = false;

function deliver(p) {
  if (p.v !== 2 || typeof p.cells !== 'string' || p.cells.length !== 1050) return;
  /* staleness disclosure, not a fallback: if the payload itself is old
   * (publisher down, machine asleep), the dot must not stay green */
  var health = typeof p.health === 'number' ? p.health : 2;
  if (p.gen && Date.now() - Date.parse(p.gen) > 20 * 60 * 1000) health = 2;
  /* the watchface relaunches on every trip into the system UI, so send once
   * per pkjs session even when gen is unchanged — and re-send whenever the
   * effective health changed (a stale payload has an unchanging gen) */
  if (sentThisSession && p.gen && p.gen === localStorage.getItem('gen') &&
      String(health) === localStorage.getItem('health')) return;

  var cells = [];
  for (var i = 0; i < 525; i++)
    cells.push(parseInt(p.cells.substr(i * 2, 2), 16));

  var vecs = [], n = 0;
  (p.vecs || []).slice(0, 6).forEach(function (v) {
    vecs.push(v[0] & 0xff, v[1] & 0xff, (v[2] + 64) & 0xff, (v[3] + 64) & 0xff);
    n++;
  });
  if (!n) vecs = [0, 0, 0, 0]; /* appmessage dislikes empty byte arrays */

  var msg = {
    CELLS: cells,
    VECS: vecs,
    NVEC: n,
    TEMP: typeof p.temp === 'number' ? Math.round(p.temp) : -999,
    DEW: typeof p.dew === 'number' ? Math.round(p.dew) : -999,
    UV: typeof p.uv === 'number' ? Math.round(p.uv) : -999,
    AQI: typeof p.aqi === 'number' ? Math.round(p.aqi) : -999,
    WIND: p.wind || '',
    HEALTH: health
  };
  Pebble.sendAppMessage(msg, function () {
    sentThisSession = true;
    localStorage.setItem('gen', p.gen || '');
    localStorage.setItem('health', String(health));
  }, function (e) {
    console.log('purple: send failed');
  });
}

Pebble.addEventListener('ready', function () {
  fetchPayload();
  setInterval(fetchPayload, POLL_MS);
});
