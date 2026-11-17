#!/bin/sh
# Downloads a URL into a file, retrying any failure and any stalled transfer
# and resuming each time. A source under the GNU mirror comes from the other
# GNU mirrors when that one keeps failing. The caller checks the checksum.
#
# Usage: fetch.sh <gnu-mirror> <file> <url>
set -u

mirror="$1"
file="$2"
url="$3"

# A transfer slower than 1 KB/s for 30 seconds counts as stalled
fetch() {
    curl -fL --retry 5 --retry-delay 3 --retry-all-errors \
        --speed-limit 1024 --speed-time 30 -C - -o "$file" "$1"
}

path="${url#"$mirror"/}"
if [ "$path" = "$url" ]; then
    fetch "$url"
    exit
fi

for site in "$mirror" https://ftp.gnu.org/gnu https://ftpmirror.gnu.org/gnu; do
    fetch "$site/$path" && exit 0
done
exit 1
