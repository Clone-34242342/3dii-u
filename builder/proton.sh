#!/usr/bin/env bash
# Launch the packaged Windows Builder with Steam Proton, in its own prefix.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exe=${THREEDII_BUILDER_EXE:-"$here/3Dii U Builder.exe"}
proton=${THREEDII_PROTON:-}
steam=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-"$HOME/.local/share/Steam"}
if [[ -z "$proton" ]]; then
    for base in "$steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        candidate="$base/steamapps/common/Proton 11.0/proton"
        if [[ -x "$candidate" ]]; then
            proton=$candidate
            steam=$base
            break
        fi
    done
fi
if [[ ! -x "$proton" || ! -f "$exe" ]]; then
    echo 'Install Steam Proton 11.0 and keep this launcher beside 3Dii U Builder.exe.' >&2
    echo 'For another Steam library, set THREEDII_PROTON to its proton script.' >&2
    exit 1
fi
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam"
export STEAM_COMPAT_DATA_PATH=${THREEDII_PROTON_DATA:-"${XDG_DATA_HOME:-$HOME/.local/share}/3dii-u-builder/proton"}
export SteamAppId=0 SteamGameId=0
mkdir -p -- "$STEAM_COMPAT_DATA_PATH"
exec "$proton" run "$exe" "$@"
