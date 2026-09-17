#!/bin/bash
# moes_fleet_ota.sh - roll a MOES TS0505B custom-firmware image out to many fixtures over the
# air, one at a time, through zigbee2mqtt, and refuse to lie about whether it worked.
#
# This is the script that updated a 49-fixture house through builds 37, 40, 43 and 44, with the
# house-specific parts (device addresses, room order) moved into arguments and files. Read
# README.md in this directory before running it; the setup it assumes is listed there.
#
#   moes_fleet_ota.sh --url URL --build v1.44s3.3 --date "20260917 06:48" [options]
#
# Required
#   --url URL        where zigbee2mqtt fetches the .zigbee image (an http server it can reach;
#                    127.0.0.1 works when zigbee2mqtt runs with host networking)
#   --build ID       the swBuildId the image reports, e.g. v1.44s3.3
#   --date STAMP     the image's dateCode build stamp, e.g. "20260917 06:48" (strings the .bin,
#                    or read it off an updated fixture). Installs are confirmed by build id OR
#                    date code, because zigbee2mqtt caches the build id across a reflash.
# Optional
#   --sha HEX        sha256 the served image must have; refuses to start on a mismatch
#   --only FILE      one IEEE address per line (0x...), in the order to update; '#' comments ok.
#                    Without it: every router on custom firmware not yet on --build.
#   --exclude "A B"  IEEE addresses never to touch (bench units, spares)
#   --gate MODE      what to check on the FIRST fixture before continuing:
#                      report  - a /get must return light_show_cue_slots (converter + build 43+)
#                      turnon  - fade the fixture off, then a raw with-on-off turn-on to level 20
#                                over 2 s must leave it ON (the build 44 fix); implies report
#                      none    - no gate (default: report)
#   --max-failures N stop after N fixtures fail 3 attempts each (default 3)
#   --dry-run        print the queue and exit
#
# Environment (defaults suit a docker-compose host with containers "zigbee2mqtt" and "mosquitto")
#   MQTT_PUB, MQTT_SUB   commands, e.g. "mosquitto_pub -h broker" / "mosquitto_sub -h broker"
#   Z2M_LOGS             command printing zigbee2mqtt's log, default "docker logs zigbee2mqtt"
#   Z2M_BASE             base topic, default zigbee2mqtt
#   LOG                  log file, default ./fleet-ota-<build>.log
set -u

URL=""; TARGET=""; TARGET_DATE=""; EXPECTED_SHA=""; ONLY=""; EXCLUDE=""; GATE="report"; MAX_FAILURES=3; DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --url) URL="$2"; shift 2 ;;
    --build) TARGET="$2"; shift 2 ;;
    --date) TARGET_DATE="$2"; shift 2 ;;
    --sha) EXPECTED_SHA="$2"; shift 2 ;;
    --only) ONLY="$2"; shift 2 ;;
    --exclude) EXCLUDE="$2"; shift 2 ;;
    --gate) GATE="$2"; shift 2 ;;
    --max-failures) MAX_FAILURES="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$URL" ] && [ -n "$TARGET" ] && [ -n "$TARGET_DATE" ] || { echo "need --url, --build and --date (see --help)" >&2; exit 2; }
case "$(basename "$URL")" in
  1141-*|*CONVERT*) echo "refusing: that looks like a CONVERT (stock-to-custom) image. This tool is custom-to-custom only; a conversion needs the procedure in docs/moes_ts0505b_conversion.md and a permit-join window." >&2; exit 2 ;;
esac

MQTT_PUB="${MQTT_PUB:-docker exec mosquitto mosquitto_pub -h localhost}"
MQTT_SUB="${MQTT_SUB:-docker exec mosquitto mosquitto_sub -h localhost}"
Z2M_LOGS="${Z2M_LOGS:-docker logs zigbee2mqtt}"
Z2M_BASE="${Z2M_BASE:-zigbee2mqtt}"
LOG="${LOG:-./fleet-ota-$TARGET.log}"
PER_DEVICE_TIMEOUT=$((60*60))   # one transfer takes 20-40 min at zigbee2mqtt's default block delay
STALL_SECS=$((6*60))            # no progress for this long = stalled; re-send resumes from the checkpoint

log(){ echo "$(date '+%H:%M:%S') [fleet-ota] $*" | tee -a "$LOG"; }
pub(){ $MQTT_PUB -t "$1" -m "$2" 2>/dev/null; }
devs_json(){ $MQTT_SUB -t "$Z2M_BASE/bridge/devices" -C 1 -W 15 2>/dev/null; }
field_of(){ devs_json | python3 -c "
import sys,json
try: ds=json.load(sys.stdin)
except Exception: sys.exit()
for d in ds:
    if d.get('ieee_address')=='$1': print(d.get('$2') or ''); break
"; }
build_of(){ field_of "$1" software_build_id; }
date_of(){ field_of "$1" date_code; }
fname_of(){ local n; n=$(field_of "$1" friendly_name); echo "${n:-$1}"; }
is_done(){ [ "$(build_of "$1")" = "$TARGET" ] || [ "$(date_of "$1")" = "$TARGET_DATE" ]; }

confirm(){   # only a forced interview refreshes zigbee2mqtt's cached build id and date code, ~75 s
  local dev="$1" i
  for i in 1 2 3; do
    pub "$Z2M_BASE/bridge/request/device/interview" "{\"id\":\"$dev\"}"
    sleep 75
    is_done "$dev" && return 0
    log "  $dev still reports build '$(build_of "$dev")' / date '$(date_of "$dev")' after interview $i"
  done
  return 1
}

FAILED=""
update_one(){
  local DEV="$1" ATTEMPT FN CUR
  CUR=$(build_of "$DEV")
  if is_done "$DEV"; then log "$DEV already on $TARGET (build '${CUR:-none}', date '$(date_of "$DEV")') - skipping"; return 0; fi
  case "$CUR" in
    v1.*) ;;
    *) log "$DEV is NOT on custom firmware (reports '${CUR:-stock}') - REFUSING; a stock fixture needs the CONVERT image and procedure"; FAILED="$FAILED $DEV"; return 1 ;;
  esac
  FN=$(fname_of "$DEV")
  local PAT="OTA update of '($DEV|$FN)' failed|failed with reason: ABORT"
  for ATTEMPT in 1 2 3; do
    log "--- $DEV ($FN) : $CUR -> $TARGET (attempt $ATTEMPT) ---"
    local BASEF; BASEF=$($Z2M_LOGS --since 30m 2>&1 | grep -acE "$PAT" || true)
    pub "$Z2M_BASE/bridge/request/device/ota_update/update" "{\"id\":\"$DEV\",\"url\":\"$URL\"}"
    local START; START=$(date +%s); local DONE=0 GAVEUP=0 LASTPCT=""
    local LASTMOVE; LASTMOVE=$(date +%s)
    while [ $(( $(date +%s) - START )) -lt $PER_DEVICE_TIMEOUT ]; do
      sleep 30
      local PCT; PCT=$($Z2M_LOGS --since 2m 2>&1 | grep -a "$DEV" | grep -ao '"progress":[0-9.]*' | tail -1)
      if [ -n "$PCT" ] && [ "$PCT" != "$LASTPCT" ]; then log "  $DEV $PCT"; LASTPCT="$PCT"; LASTMOVE=$(date +%s); fi
      # A transfer can stop advancing with no failure logged; re-sending resumes from the checkpoint.
      IS100=$(awk -v p="${LASTPCT:-0}" 'BEGIN{gsub(/[^0-9.]/,"",p); print (p>=99.9)?1:0}')
      if [ "$IS100" = "0" ] && [ $(( $(date +%s) - LASTMOVE )) -ge $STALL_SECS ]; then
        log "  $DEV no progress for $((STALL_SECS/60)) min (stalled at ${LASTPCT:-?}) - re-sending"; GAVEUP=1; break
      fi
      is_done "$DEV" && { DONE=1; log "$DEV reports $TARGET"; break; }
      if $Z2M_LOGS --since 3m 2>&1 | grep -aq "Update of '\?$DEV'\? successful\|Update of '\?$FN'\? successful"; then
        confirm "$DEV" && { DONE=1; log "$DEV confirmed $TARGET"; break; }
      fi
      local NOWF; NOWF=$($Z2M_LOGS --since 30m 2>&1 | grep -acE "$PAT" || true)
      if [ "${NOWF:-0}" -gt "${BASEF:-0}" ]; then log "  $DEV transfer failed/aborted - will retry (resumes from checkpoint)"; GAVEUP=1; break; fi
    done
    [ "$DONE" = "1" ] && { log "$DEV OK"; sleep 20; return 0; }
    if [ "$GAVEUP" = "1" ]; then sleep 60; CUR=$(build_of "$DEV"); continue; fi
    log "$DEV no confirmation within budget - forcing an interview"
    confirm "$DEV" && { log "$DEV OK"; sleep 20; return 0; }
  done
  log "$DEV FAILED after 3 attempts"; FAILED="$FAILED $DEV"; sleep 20; return 1
}

# ---- the queue ----
fleet_list(){
  if [ -n "$ONLY" ]; then
    grep -oE '^\s*0x[0-9a-fA-F]{16}' "$ONLY" | tr -d ' \t' | tr 'A-F' 'a-f'
  else
    devs_json | EXCLUDE="$EXCLUDE" TARGET="$TARGET" TARGET_DATE="$TARGET_DATE" python3 -c '
import sys, json, re, os
ex = set(os.environ["EXCLUDE"].split()); target = os.environ["TARGET"]; target_date = os.environ["TARGET_DATE"]
try: ds = json.load(sys.stdin)
except Exception: sys.exit()
out = []
for d in ds:
    if d.get("type") != "Router" or d.get("ieee_address") in ex: continue
    if d.get("date_code") == target_date: continue
    b = d.get("software_build_id") or ""
    if re.match(r"^v1\.\d+s3\.3$", b) and b != target:
        out.append(((d.get("description") or ""), d["ieee_address"]))
for desc, i in sorted(out): print(i)
'
  fi
}
ALL=$(fleet_list | while read -r d; do case " $EXCLUDE " in *" $d "*) ;; *) echo "$d" ;; esac; done)
if [ "$DRY" = 1 ]; then
  echo "would update $(echo $ALL | wc -w) fixtures with $URL -> $TARGET ($TARGET_DATE), in this order:"
  for D in $ALL; do echo "  $D  $(fname_of "$D")  build '$(build_of "$D")' date '$(date_of "$D")'"; done
  exit 0
fi

# ---- preflight: one run at a time, the right bytes, a live zigbee2mqtt, nothing in flight ----
exec 9>/tmp/moes-fleet-ota.lock
flock -n 9 || { echo "another rollout holds /tmp/moes-fleet-ota.lock - never run two OTAs at once" >&2; exit 1; }
if [ -n "$EXPECTED_SHA" ]; then
  GOT=$(curl -s "$URL" | sha256sum | cut -c1-64)
  [ "$GOT" = "$EXPECTED_SHA" ] || { echo "served image sha256 $GOT != expected $EXPECTED_SHA - refusing" >&2; exit 1; }
fi
devs_json | python3 -c "import sys,json; ds=json.load(sys.stdin); sys.exit(0 if len(ds)>0 else 1)" 2>/dev/null \
  || { echo "zigbee2mqtt bridge/devices not answering - refusing" >&2; exit 1; }
if $Z2M_LOGS --since 3m 2>&1 | grep -aq '"progress":[0-9]'; then echo "an OTA transfer appears to be in flight - refusing" >&2; exit 1; fi
[ -n "$ALL" ] || { log "nothing to do: no fixture on custom firmware below $TARGET"; exit 0; }

log "=== $(echo $ALL | wc -w) fixtures -> $TARGET ($TARGET_DATE), one at a time ==="
log "url $URL"
for D in $ALL; do log "  queued $D ($(fname_of "$D"))"; done

gate(){   # returns 0 to continue, 1 to stop
  local D="$1" FN SUB i; FN=$(fname_of "$D")
  [ "$GATE" = "none" ] && return 0
  # The fixture has just rebooted into the new build. Wait for it to answer a plain read (up to
  # 3 min) before asking anything of it: a one-shot 20 s window here once read an empty report
  # from a healthy fixture and stopped a run for five hours.
  local ALIVE=0
  for i in $(seq 1 12); do
    sleep 15
    $MQTT_SUB -t "$Z2M_BASE/$FN" -C 1 -W 15 > /tmp/fleet-ota-alive.json 2>/dev/null & SUB=$!; sleep 2
    pub "$Z2M_BASE/$FN/get" '{"state":""}'; wait $SUB
    grep -q '"state"' /tmp/fleet-ota-alive.json 2>/dev/null && { ALIVE=1; log "  $D answers reads again after ~$((i*17)) s"; break; }
  done
  [ "$ALIVE" = 1 ] || log "  $D not answering reads 3 min after its install - trying the report anyway"
  local OK1=0
  for i in 1 2 3 4 5 6; do
    : > /tmp/fleet-ota-gate1.json
    $MQTT_SUB -t "$Z2M_BASE/$FN" -C 1 -W 20 > /tmp/fleet-ota-gate1.json 2>/dev/null & SUB=$!; sleep 2
    pub "$Z2M_BASE/$FN/get" '{"light_show":""}'; wait $SUB
    grep -q '"light_show_cue_slots"' /tmp/fleet-ota-gate1.json 2>/dev/null && { OK1=1; break; }
    log "  gate report try $i: nothing yet - retrying in 20 s"; sleep 20
  done
  if [ "$OK1" = 1 ]; then
    log "GATE report OK on $D ($FN): $(grep -o '"light_show_cue_slots":"[^"]*"' /tmp/fleet-ota-gate1.json)"
  else
    log "GATE report FAILED on $D ($FN): the fixture never reported light_show_cue_slots - is the converter from this tag installed? STOPPING"; return 1
  fi
  [ "$GATE" = "turnon" ] || return 0
  local OFF='{"command":{"cluster":"genLevelCtrl","command":"moveToLevelWithOnOff","payload":{"level":0,"transtime":20}}}'
  local ON20='{"command":{"cluster":"genLevelCtrl","command":"moveToLevelWithOnOff","payload":{"level":20,"transtime":20}}}'
  pub "$Z2M_BASE/$FN/set" "$OFF"; sleep 4
  pub "$Z2M_BASE/$FN/set" "$ON20"; sleep 4
  $MQTT_SUB -t "$Z2M_BASE/$FN" -C 1 -W 20 > /tmp/fleet-ota-gate2.json 2>/dev/null & SUB=$!; sleep 2
  pub "$Z2M_BASE/$FN/get" '{"state":""}'; wait $SUB
  if grep -q '"state":"ON"' /tmp/fleet-ota-gate2.json 2>/dev/null; then
    log "GATE turnon OK on $D ($FN): a dim turn-on with a fade from a faded-off fixture stays ON"
    pub "$Z2M_BASE/$FN/set" "$OFF"; sleep 3; return 0
  fi
  log "GATE turnon FAILED on $D ($FN): read back $(grep -o '"state":"[A-Z]*"' /tmp/fleet-ota-gate2.json 2>/dev/null) - STOPPING"
  pub "$Z2M_BASE/$FN/set" "$OFF"; return 1
}

FIRST=1; GATE_STOPPED=0; STOPPED_ON_FAILURES=0
for D in $ALL; do
  update_one "$D"
  N=$(echo $FAILED | wc -w)
  if [ "$N" -ge "$MAX_FAILURES" ]; then log "STOPPING: $N failures ($FAILED)"; STOPPED_ON_FAILURES=1; break; fi
  if [ "$FIRST" = 1 ]; then FIRST=0; gate "$D" || { GATE_STOPPED=1; break; }; fi
done
log "=== RUN FINISHED ==="
for D in $ALL; do log "  $D -> build '$(build_of "$D")' date '$(date_of "$D")'"; done
if [ "$GATE_STOPPED" = 1 ]; then log "RESULT: stopped at the gate after the first fixture - the rest were NOT updated"
elif [ "$STOPPED_ON_FAILURES" = 1 ]; then log "RESULT: stopped after $MAX_FAILURES failed fixtures ($FAILED) - the rest were NOT updated"
elif [ -n "$FAILED" ]; then log "RESULT: finished; FAILED:$FAILED"
else log "RESULT: every queued fixture is on $TARGET"; fi
