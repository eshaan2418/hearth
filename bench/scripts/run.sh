#!/usr/bin/env bash
# hearth vs nginx: same files, same worker count, keep-alive, 5 interleaved runs each.
# Usage: bench/scripts/run.sh <build-dir> <out.jsonl>
set -euo pipefail
BUILD=${1:?}; OUT=${2:?}; REPS=${REPS:-5}; SECS=${SECS:-5}; W=${WORKERS:-4}
T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
WWW="$T/www"; mkdir -p "$WWW"
head -c 1024 /dev/urandom > "$WWW/1k.bin"
head -c 102400 /dev/urandom > "$WWW/100k.bin"
head -c 1048576 /dev/urandom > "$WWW/1m.bin"
cat > "$T/nginx.conf" <<CONF
worker_processes $W;
pid $T/nginx.pid;
error_log $T/error.log;
events { worker_connections 8192; }
http {
  access_log off;
  sendfile on; tcp_nopush on; tcp_nodelay on;
  keepalive_timeout 60; keepalive_requests 10000000;
  open_file_cache max=1000 inactive=60s;
  client_body_temp_path $T; proxy_temp_path $T; fastcgi_temp_path $T; uwsgi_temp_path $T; scgi_temp_path $T;
  server { listen 127.0.0.1:18091; root $WWW; }
}
CONF
NGINX=$(command -v nginx)
"$NGINX" -c "$T/nginx.conf" -g 'daemon off;' 2>/dev/null &
"$BUILD/hearthd" -root "$WWW" -port 18090 -threads "$W" 2>/dev/null &
sleep 1
load1() { sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}' || cut -d' ' -f1 /proc/loadavg; }
: > "$OUT"
for spec in "1k.bin 64" "100k.bin 64" "1m.bin 16"; do
  set -- $spec; file=$1; conns=$2
  for rep in $(seq 1 "$REPS"); do
    for srv in hearth nginx; do
      port=18090; [[ $srv == nginx ]] && port=18091
      l=$(load1)
      "$BUILD/hearth-load" 127.0.0.1 "$port" "/$file" "$conns" "$SECS" | sed "s/^{/{\"server\":\"$srv\",\"rep\":$rep,\"load1\":$l,/" >> "$OUT"
    done
  done
done
echo "nginx: $("$NGINX" -v 2>&1)" > "${OUT%.jsonl}.versions.txt"
