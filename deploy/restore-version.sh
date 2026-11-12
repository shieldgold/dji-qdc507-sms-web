#!/bin/sh
# Restore only the previous executable; preserve current messages and passwords.
set -eu
umask 077
D=/data/smsweb
[ -s "$D/previous/smsweb" ] && [ -x "$D/previous/smsweb" ]
sleep 2
sh "$D/start.sh" stop
cp "$D/previous/smsweb" "$D/smsweb.restore"
chmod 700 "$D/smsweb.restore"
sync
mv "$D/smsweb" "$D/smsweb.replaced"
mv "$D/smsweb.restore" "$D/smsweb"
sync
sh "$D/start.sh"
