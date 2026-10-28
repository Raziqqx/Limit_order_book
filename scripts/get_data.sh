#!/usr/bin/env bash
# Download one full trading day of NASDAQ TotalView-ITCH 5.0 sample data and decompress it.
# Defaults to 2019-12-30 (the smallest full day on the server, ~3.5 GB gz / ~8 GB raw).
# Keep the data on the Linux filesystem (not /mnt/c under WSL): it is several times faster.
set -euo pipefail
DAY="${1:-12302019}"
DIR="${2:-$HOME/itch-data}"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DAY}.NASDAQ_ITCH50.gz"
mkdir -p "$DIR"
cd "$DIR"
echo "downloading $URL -> $DIR"
curl -fL -C - -o "${DAY}.NASDAQ_ITCH50.gz" "$URL"
echo "checking gzip integrity"
gzip -t "${DAY}.NASDAQ_ITCH50.gz"
echo "decompressing"
gunzip -kf "${DAY}.NASDAQ_ITCH50.gz"
ls -la "$DIR"
