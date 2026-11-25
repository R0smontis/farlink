#!/bin/bash
# Only run the installed binary on the live system volume. Use each user's
# identity to read their settings and update their own LaunchAgent.
[[ "${3:-}" == / ]] || exit 0
executable="${2%/}/Farlink.app/Contents/MacOS/Farlink"
[[ -x "$executable" ]] || exit 0
while read -r account uid; do
    [[ "$uid" =~ ^[0-9]+$ && "$uid" -ge 500 && "$account" != nobody ]] || continue
    /usr/bin/sudo -n -H -u "#$uid" -- /bin/sh -c '
        if [ -f "$HOME/.cache/Farlink/config.ini" ] ||
           [ -f "$HOME/Library/LaunchAgents/Farlink.plist" ]; then
            exec "$1" --autostart-migrate
        fi' sh "$executable" ||
        echo "Farlink will retry startup migration on the next launch for $account." >&2
done < <(/usr/bin/dscl . -list /Users UniqueID)
exit 0
