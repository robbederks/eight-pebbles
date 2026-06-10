/* Eight Sleep API client for the watch app.
 *
 * Runs on the phone. Talks to the unofficial Eight Sleep API (as
 * reverse-engineered by github.com/lukas-clarke/eight_sleep):
 *   - auth-api.8slp.net  : OAuth2 password grant (no refresh-token grant;
 *                          "refresh" = log in again)
 *   - client-api.8slp.net: account/device/side lookup
 *   - app-api.8slp.net   : temperature state + control, keyed by side userId
 *
 * Credentials come from the Clay config page and never leave the phone
 * except to Eight Sleep's auth endpoint.
 *
 * Every command from the watch carries a Seq number; the reply echoes it so
 * the watch can tell the real reply apart from unsolicited status pushes.
 */

var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

var AUTH_URL = 'https://auth-api.8slp.net/v1/tokens';
var APP_API = 'https://app-api.8slp.net/v1';
var APP_API_V2 = 'https://app-api.8slp.net/v2';
var CLIENT_API = 'https://client-api.8slp.net/v1';

// Public client credentials extracted from the official Eight Sleep app,
// shared by all open-source integrations. May rotate with app updates.
var CLIENT_ID = '0894c7f33bb94800a03f1f4df13a4f38';
var CLIENT_SECRET =
  'f0954a3ed5763ba3d06834c73731a32f15f168f47d4f164751275def86db0c76';

var TOKEN_BUFFER_S = 120;
var XHR_TIMEOUT_MS = 6000;

var ERR_NO_CREDENTIALS = 1;
var ERR_AUTH_FAILED = 2;
var ERR_NETWORK = 3;
var ERR_API = 4;

var busy = false;
// Two queue slots so an alarm dismiss/snooze is never displaced by a
// temperature command (or vice versa); latest wins within each domain.
var queuedAlarm = null;
var queuedBed = null;
// Highest command seq received from the watch; heartbeats echo it so the
// watch keeps its timeout armed even while an older command's chain runs.
var lastSeqReceived = 0;
var heartbeatTimer = null;
// Exception safety for the command chain: every async continuation runs
// through chainGuard, which fails the command instead of wedging `busy`
// (and with it the heartbeat, which would suppress the watch's timeout
// forever). lastProgress doubles as the heartbeat's dead-chain cutoff.
var currentSeq = 0;
var lastProgress = 0;
var chainFinished = false;

function chainGuard(fn) {
  return function () {
    lastProgress = Date.now();
    try {
      return fn.apply(this, arguments);
    } catch (e) {
      console.log('uncaught in command chain: ' + e);
      if (busy && !chainFinished) {
        chainFinished = true;
        try {
          reportError(ERR_API, currentSeq);
        } catch (e2) { /* reporting is best-effort */ }
        try {
          finishCmd();
        } catch (e3) {
          busy = false;
          stopHeartbeat();
        }
      }
    }
  };
}
// Bumped on every settings save: async callbacks from before the save must
// not repopulate the caches that clearCaches() just wiped.
var settingsGen = 0;
// Bumped on every send to the watch: a retry of an older status must not
// overtake and revert a newer one.
var sendGen = 0;

function getSettings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

function rawValue(v) {
  return (v && typeof v === 'object' && 'value' in v) ? v.value : v;
}

function getCredentials() {
  var s = getSettings();
  return {
    email: (rawValue(s.Email) || '').trim(),
    password: rawValue(s.Password) || '',
    sideOverride: rawValue(s.SideOverride) || 'auto',
    unitC: rawValue(s.Unit) !== 'f',
    haptics: rawValue(s.Haptics) === undefined ? true : !!rawValue(s.Haptics),
    scaleDeg: rawValue(s.Scale) === 'deg',
    snoozeMin: parseInt(rawValue(s.SnoozeMin), 10) || 10
  };
}

function isDemo() {
  return getCredentials().email.toLowerCase() === 'demo';
}

function xhr(method, url, auth, body, cb) {
  var req = new XMLHttpRequest();
  req.open(method, url);
  req.timeout = XHR_TIMEOUT_MS;
  req.setRequestHeader('Content-Type', 'application/json');
  req.setRequestHeader('Accept', 'application/json');
  if (auth) {
    req.setRequestHeader('Authorization', 'Bearer ' + auth);
  }
  req.onload = chainGuard(function () {
    var data = null;
    try {
      data = JSON.parse(req.responseText);
    } catch (e) { /* some 2xx replies have no body */ }
    cb(null, req.status, data);
  });
  req.onerror = chainGuard(function () { cb(ERR_NETWORK); });
  req.ontimeout = chainGuard(function () { cb(ERR_NETWORK); });
  req.send(body ? JSON.stringify(body) : null);
}

// ---- Auth ----

function loadToken() {
  try {
    return JSON.parse(localStorage.getItem('es_token'));
  } catch (e) {
    return null;
  }
}

function clearCaches() {
  localStorage.removeItem('es_token');
  localStorage.removeItem('es_side');
}

function ensureToken(cb) {
  var gen = settingsGen;
  var creds = getCredentials();
  if (!creds.email || !creds.password) {
    return cb(ERR_NO_CREDENTIALS);
  }
  var tok = loadToken();
  var now = Date.now() / 1000;
  if (tok && tok.access && now + TOKEN_BUFFER_S < tok.exp) {
    return cb(null, tok);
  }
  xhr('POST', AUTH_URL, null, {
    client_id: CLIENT_ID,
    client_secret: CLIENT_SECRET,
    grant_type: 'password',
    username: creds.email,
    password: creds.password
  }, function (err, status, data) {
    if (err) return cb(err);
    if (status !== 200 || !data || !data.access_token) {
      console.log('auth failed: HTTP ' + status);
      // 5xx/429 are auth-server trouble, not bad credentials (those are
      // 400/401): show the retryable NO CONNECTION, not CHECK LOGIN.
      return cb(status >= 500 || status === 429 ? ERR_NETWORK
                                                : ERR_AUTH_FAILED);
    }
    var newTok = {
      access: data.access_token,
      exp: Date.now() / 1000 + (data.expires_in || 3600),
      userId: data.userId
    };
    if (gen === settingsGen) {
      localStorage.setItem('es_token', JSON.stringify(newTok));
    }
    cb(null, newTok);
  });
}

// Authenticated request with a single re-auth retry on 401.
// cb(err, data, httpStatus) — status lets callers special-case e.g. 409.
function apiCall(method, url, body, cb, isRetry) {
  ensureToken(function (err, tok) {
    if (err) return cb(err);
    xhr(method, url, tok.access, body, function (xerr, status, data) {
      if (xerr) return cb(xerr);
      if (status === 401 && !isRetry) {
        localStorage.removeItem('es_token');
        return apiCall(method, url, body, cb, true);
      }
      if (status < 200 || status >= 300) {
        console.log(method + ' ' + url + ' -> HTTP ' + status);
        return cb(status === 401 ? ERR_AUTH_FAILED : ERR_API, data, status);
      }
      cb(null, data, status);
    });
  });
}

// ---- Side resolution ----

function resolveSide(cb, force) {
  var gen = settingsGen;
  var creds = getCredentials();
  var cached = null;
  try {
    cached = JSON.parse(localStorage.getItem('es_side'));
  } catch (e) { /* ignore */ }
  if (!force && cached && cached.uid && cached.override === creds.sideOverride) {
    return cb(null, cached);
  }

  function store(uid, name) {
    var side = { uid: uid, name: name, override: creds.sideOverride };
    if (gen === settingsGen) {
      localStorage.setItem('es_side', JSON.stringify(side));
    }
    cb(null, side);
  }

  if (creds.sideOverride === 'auto') {
    ensureToken(function (terr, tok) {
      if (terr) return cb(terr);
      if (!tok.userId) return cb(ERR_AUTH_FAILED);
      apiCall('GET', CLIENT_API + '/users/' + tok.userId + '/current-device',
        null,
        function (err, data) {
          if (err) {
            // The side name is cosmetic. Use a fallback for this command but
            // don't cache it, so the next command retries the lookup.
            return cb(null, { uid: tok.userId, name: 'MY SIDE',
                              override: creds.sideOverride });
          }
          var side = data && (data.side || (data.result && data.result.side));
          store(tok.userId, side ? side.toUpperCase() : 'MY SIDE');
        });
    });
    return;
  }

  // Explicit left/right: look the userId up via the device record
  apiCall('GET', CLIENT_API + '/users/me', null, function (err, data) {
    if (err) return cb(err);
    var user = data && data.user;
    var deviceId = user && user.devices && user.devices[0];
    if (!deviceId) return cb(ERR_API);
    apiCall('GET', CLIENT_API + '/devices/' + deviceId +
      '?filter=leftUserId,rightUserId,awaySides', null,
      function (derr, ddata) {
        if (derr) return cb(derr);
        var r = (ddata && (ddata.result || ddata)) || {};
        var key = creds.sideOverride + 'UserId';
        var uid = r[key] || (r.awaySides && r.awaySides[key]);
        if (!uid) return cb(ERR_API);
        store(uid, creds.sideOverride.toUpperCase());
      });
  });
}

// ---- Temperature state + control ----

function parseTempState(data) {
  var d = (data && (data.result || data)) || {};
  var type = (d.currentState && d.currentState.type) || 'off';
  var on = type !== 'off';
  var phase = type.indexOf(':') > 0 ? type.split(':')[1] : '';
  return {
    level: typeof d.currentLevel === 'number' ? d.currentLevel : 0,
    deviceLevel:
      typeof d.currentDeviceLevel === 'number' ? d.currentDeviceLevel : 0,
    on: on,
    phase: phase
  };
}

function fetchStatus(side, cb) {
  apiCall('GET', APP_API + '/users/' + side.uid + '/temperature', null,
    function (err, data) {
      if (err) return cb(err);
      cb(null, parseTempState(data));
    });
}

function putTemperature(side, body, cb) {
  apiCall('PUT', APP_API + '/users/' + side.uid + '/temperature', body, cb);
}

// ---- Alarms ----

// "Active" = ringing right now (within its start..end window) or snoozed.
function findActiveAlarm(alarms) {
  var now = Date.now();
  for (var i = 0; i < alarms.length; i++) {
    var a = alarms[i];
    if (a.snoozing) return a;
    var s = a.startTimestamp ? Date.parse(a.startTimestamp) : NaN;
    var e = a.endTimestamp ? Date.parse(a.endTimestamp) : NaN;
    if (!isNaN(s) && !isNaN(e) && s <= now && now <= e) return a;
  }
  return null;
}

function fetchActiveAlarm(side, cb) {
  apiCall('GET', APP_API_V2 + '/users/' + side.uid + '/alarms', null,
    function (err, data) {
      if (err) return cb(err);
      var d = (data && (data.result || data)) || {};
      cb(null, findActiveAlarm(d.alarms || []));
    });
}

function dismissAlarm(side, alarm, cb) {
  apiCall('PUT',
    APP_API + '/users/' + side.uid + '/alarms/' + alarm.id + '/dismiss',
    { ignoreDeviceErrors: false },
    function (err, data, status) {
      // 409 = alarm wasn't ringing (anymore): the outcome the user wanted
      if (err && status !== 409) return cb(err);
      cb(null);
    });
}

function snoozeAlarm(side, alarm, minutes, cb) {
  apiCall('PUT',
    APP_API + '/users/' + side.uid + '/alarms/' + alarm.id + '/snooze',
    { snoozeMinutes: minutes, ignoreDeviceErrors: false },
    function (err, data, status) {
      if (err && status !== 409) return cb(err);
      cb(null);
    });
}

// 0 = none, 1 = ringing, 2 = snoozed (matches the C ALARM_* enum)
function alarmStateOf(active) {
  if (!active) return 0;
  return active.snoozing ? 2 : 1;
}

// ---- Watch messaging ----

function sendToWatch(dict, retried) {
  var gen = ++sendGen;
  Pebble.sendAppMessage(dict, function () {}, function () {
    if (!retried) {
      setTimeout(function () {
        if (gen === sendGen) {
          sendToWatch(dict, true);
        }
      }, 300);
    }
  });
}

function reportStatus(side, st, seq, pendingBed) {
  var creds = getCredentials();
  var dict = {
    Configured: 1,
    Error: 0,
    Seq: seq || 0,
    DeviceLevel: st.deviceLevel,
    Unit: creds.unitC ? 1 : 0,
    Scale: creds.scaleDeg ? 1 : 0,
    SideName: side && side.name ? side.name : 'MY SIDE',
    Haptics: creds.haptics ? 1 : 0
  };
  // A queued bed command is about to change these; omitting them keeps the
  // watch's optimistic values instead of visibly reverting a nudge that
  // hasn't run yet. The bed command's own status delivers the real numbers.
  if (!pendingBed) {
    dict.State = st.on ? 1 : 0;
    dict.Phase = st.phase || '';
    dict.Level = st.level;
  } else if (pendingBed.type === 'power') {
    dict.Level = st.level;  // a power toggle doesn't change the level
  }
  if (typeof st.alarm !== 'undefined') {
    // Omitted when the alarm check failed, so the watch keeps its last value
    dict.Alarm = st.alarm;
  }
  sendToWatch(dict);
}

function reportError(code, seq) {
  sendToWatch({
    Configured: code === ERR_NO_CREDENTIALS ? 0 : 1,
    Error: code,
    Seq: seq || 0
  });
}

// ---- Demo mode (email: "demo") for trying the UI without an account ----

function demoState() {
  var st;
  try {
    st = JSON.parse(localStorage.getItem('es_demo'));
  } catch (e) { /* ignore */ }
  return st ||
    { level: -20, deviceLevel: -10, on: true, phase: 'bedtime', alarm: false };
}

function runDemo(cmd) {
  var st = demoState();
  st.alarm = st.alarm === true ? 1 : (st.alarm || 0);
  if (cmd.type === 'level') {
    st.level = cmd.level;
    st.on = true;
  } else if (cmd.type === 'power') {
    st.on = cmd.on;
  } else if (cmd.type === 'alarm') {
    st.alarm = 0;
  } else if (cmd.type === 'snooze') {
    if (st.alarm === 1) st.alarm = 2;
  }
  localStorage.setItem('es_demo', JSON.stringify(st));
  setTimeout(chainGuard(function () {
    reportStatus({ name: 'DEMO' }, st, cmd.seq, queuedBed);
    finishCmd();
  }), st.delay || 600);  // st.delay: simulate a slow API for UI testing
}

// ---- Command loop (one in flight, type-aware latest-wins queue) ----

function isAlarmCmd(cmd) {
  return cmd.type === 'alarm' || cmd.type === 'snooze';
}

// Fire-and-forget (no retry, no sendGen bump): a lost heartbeat is
// recovered by the next one.
function sendHeartbeat() {
  if (Date.now() - lastProgress > 60000) {
    // No chain progress for a minute: something escaped the guards. Stop
    // keeping the watch alive so its own timeout can surface the failure.
    stopHeartbeat();
    return;
  }
  if (lastSeqReceived) {
    Pebble.sendAppMessage({ Seq: lastSeqReceived, Working: 1 },
      function () {}, function () {});
  }
}

function startHeartbeat() {
  if (!heartbeatTimer) {
    heartbeatTimer = setInterval(sendHeartbeat, 8000);
  }
}

function stopHeartbeat() {
  if (heartbeatTimer) {
    clearInterval(heartbeatTimer);
    heartbeatTimer = null;
  }
}

function finishCmd() {
  busy = false;
  var next = queuedAlarm || queuedBed;
  if (next) {
    if (next === queuedAlarm) {
      queuedAlarm = null;
    } else {
      queuedBed = null;
    }
    runCmd(next);
  } else {
    stopHeartbeat();
  }
}

function runCmd(cmd) {
  if (busy) {
    if (isAlarmCmd(cmd)) {
      queuedAlarm = cmd;
    } else if (!(cmd.soft && queuedBed)) {
      // A soft (unsequenced, internal) refresh must never displace a queued
      // seq'd watch command; that command's reply carries the fresh
      // settings anyway.
      queuedBed = cmd;
    }
    return;
  }
  busy = true;
  currentSeq = cmd.seq || 0;
  chainFinished = false;
  lastProgress = Date.now();

  chainGuard(function () {
    startHeartbeat();
    // Progress ack: tells the watch the phone is alive and on it, so it can
    // budget the network timeout instead of the phone-dead timeout.
    if (cmd.seq) {
      sendToWatch({ Seq: cmd.seq, Working: 1 });
    }

    if (isDemo()) {
      return runDemo(cmd);
    }

    runRealCmd(cmd);
  })();
}

function runRealCmd(cmd) {
  resolveSide(function (err, side) {
    if (err) {
      reportError(err, cmd.seq);
      return finishCmd();
    }

    function fail(ferr) {
      if (ferr === ERR_API) {
        // The cached side mapping may be stale (side swap, away mode, device
        // change); re-resolve on the next command.
        localStorage.removeItem('es_side');
      }
      reportError(ferr, cmd.seq);
      finishCmd();
    }

    function refresh() {
      fetchStatus(side, function (serr, st) {
        if (serr) return fail(serr);
        fetchActiveAlarm(side, function (aerr, active) {
          if (!aerr) {
            st.alarm = alarmStateOf(active);
          }
          // Alarm check failing is not worth failing the whole refresh over
          reportStatus(side, st, cmd.seq, queuedBed);
          finishCmd();
        });
      });
    }

    if (cmd.type === 'refresh') {
      refresh();
    } else if (cmd.type === 'alarm') {
      fetchActiveAlarm(side, function (aerr, active) {
        if (aerr) return fail(aerr);
        if (!active) return refresh();  // already over: just resync
        dismissAlarm(side, active, function (derr) {
          if (derr) return fail(derr);
          refresh();
        });
      });
    } else if (cmd.type === 'snooze') {
      fetchActiveAlarm(side, function (aerr, active) {
        if (aerr) return fail(aerr);
        if (!active) return refresh();  // already over: just resync
        snoozeAlarm(side, active, getCredentials().snoozeMin,
          function (serr) {
            if (serr) return fail(serr);
            refresh();
          });
      });
    } else if (cmd.type === 'level') {
      // The reference integration always turns the side on before setting a
      // level; setting a level on an off side may not take effect, and our
      // last-known on/off can be stale (partner/app/schedule changes it).
      putTemperature(side, { currentState: { type: 'smart' } },
        function (perr) {
          if (perr) return fail(perr);
          putTemperature(side, { currentLevel: cmd.level }, function (perr2) {
            if (perr2) return fail(perr2);
            refresh();
          });
        });
    } else if (cmd.type === 'power') {
      putTemperature(side,
        { currentState: { type: cmd.on ? 'smart' : 'off' } },
        function (perr) {
          if (perr) return fail(perr);
          refresh();
        });
    } else {
      finishCmd();
    }
  }, cmd.force);
}

// ---- Events ----

Pebble.addEventListener('ready', function () {
  runCmd({ type: 'refresh', soft: true });
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  var seq = p.Seq || 0;
  var cmd = p.Cmd;
  if (seq) {
    lastSeqReceived = seq;
    // Immediate receipt ack — the command may queue behind another, and the
    // watch should budget for network work, not declare the phone dead.
    sendToWatch({ Seq: seq, Working: 1 });
  }
  if (cmd === 1) {
    runCmd({ type: 'level', level: p.Level, seq: seq });
  } else if (cmd === 2) {
    runCmd({ type: 'power', on: !!p.State, seq: seq });
  } else if (cmd === 3) {
    runCmd({ type: 'alarm', seq: seq });
  } else if (cmd === 4) {
    runCmd({ type: 'snooze', seq: seq });
  } else {
    // Cmd 0 (plain Select with no active alarm) or unknown Cmd: refresh,
    // forcing side re-resolution so it doubles as the recovery gesture for
    // a stale cached side.
    runCmd({ type: 'refresh', seq: seq, force: true });
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) {
    return;
  }
  // Persists to localStorage('clay-settings'); credentials stay on the phone.
  try {
    clay.getSettings(e.response, false);
  } catch (err) {
    console.log('clay settings error: ' + err);
    return;
  }
  settingsGen++;
  clearCaches();
  runCmd({ type: 'refresh', soft: true });
});
