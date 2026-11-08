#!/bin/sh
# Turn the barcode scanner's illumination off at boot, before the program that reads
# barcodes opens the port.  The setting is a runtime one, so it must be re-applied
# after every power cycle.  Reads SCANNER_DEVICE and TOOLS_DIR from the config file.
#
#   sh scanner-leds-off-at-boot.sh [CONFIG]      (default: /etc/proxmobil3-tools.conf)
set -eu
CONF=${1:-/etc/proxmobil3-tools.conf}
[ -r "$CONF" ] && . "$CONF"
: "${SCANNER_DEVICE:?set SCANNER_DEVICE in $CONF (see config/proxmobil3-tools.conf.example)}"
TOOLS_DIR=${TOOLS_DIR:-/usr/local/bin}

tries=0
while [ ! -c "$SCANNER_DEVICE" ] && [ "$tries" -lt 10 ]; do   # USB enumeration can lag behind boot
    sleep 1
    tries=$((tries + 1))
done
if [ ! -c "$SCANNER_DEVICE" ]; then
    echo "scanner not present: $SCANNER_DEVICE; illumination unchanged" >&2
    exit 1
fi
exec "$TOOLS_DIR/pm3-scanner-light" --device "$SCANNER_DEVICE" off
