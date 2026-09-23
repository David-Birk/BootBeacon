#!/bin/bash
# Liest die BootBeacon-Blackbox unter Linux aus.
#   sudo ./bblog.sh          -> anzeigen und nach ~/bootbeacon-<datum>.log kopieren
#   sudo ./bblog.sh --clear  -> Variable loeschen (vor dem naechsten Test)
VAR=/sys/firmware/efi/efivars/bootbeacon-log-7c436110-ab2a-4bbb-a880-fe41995c9f82
if [ "$1" = "--clear" ]; then
  [ -e "$VAR" ] || { echo "Keine Variable vorhanden."; exit 0; }
  chattr -i "$VAR" 2>/dev/null; rm -f "$VAR" && echo "Geloescht."
  exit
fi
if [ ! -e "$VAR" ]; then
  echo "Keine BootBeacon-Variable gefunden."
  echo "=> Kext wurde nicht geladen, EC-Match hat nicht geklappt, oder macOS darf nicht ins NVRAM schreiben."
  exit 1
fi
OUT="${SUDO_USER:+/home/$SUDO_USER}/bootbeacon-$(date +%Y%m%d-%H%M%S).log"
tail -c +5 "$VAR" | tr -d '\000' > "$OUT"
[ -n "$SUDO_USER" ] && chown "$SUDO_USER" "$OUT"
head -1 "$OUT"
echo "------------------------------------------------------------"
tail -n +2 "$OUT"
echo "------------------------------------------------------------"
echo "Gespeichert: $OUT"
