#!/usr/bin/env bash
# Start hearthd on a scratch directory and show what it does over plain HTTP.
set -euo pipefail
BUILD=${1:-build}
T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/www/docs"
echo '<h1>hello from hearth</h1>' > "$T/www/index.html"
echo 'notes' > "$T/www/docs/index.html"
printf '0123456789abcdef' > "$T/www/digits.txt"
"$BUILD/hearthd" -root "$T/www" -port 18099 -threads 2 2>/dev/null &
sleep 0.5
U=http://127.0.0.1:18099
show() {
  local line='$'
  for a in "$@"; do [[ $a == *' '* ]] && line+=" '$a'" || line+=" $a"; done
  echo "$line"; "$@" 2>/dev/null | tr -d '\r' | grep -vE '^(Date|Last-Modified):'; echo; }
show curl -si $U/
ETAG=$(curl -sI $U/digits.txt | tr -d '\r' | awk '/^ETag/{print $2}')
show curl -si -H "If-None-Match: $ETAG" $U/digits.txt
show curl -si -H 'Range: bytes=4-9' $U/digits.txt
show curl -si $U/docs
show curl -si --path-as-is $U/../../etc/passwd
echo '$ hearth-load 127.0.0.1 18099 /index.html 8 2'
"$BUILD/hearth-load" 127.0.0.1 18099 /index.html 8 2
