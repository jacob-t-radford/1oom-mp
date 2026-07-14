#!/bin/bash
# Publish the current build to the PRIVATE distribution repo
# (github.com/jacob-t-radford/1oom-mp-releases -- family only; the full bundle includes game data,
# which is why it lives in a private repo and not in this public one's releases).
#
# Needs: ~/.config/1oom/release_token_rw -- a fine-grained GitHub token with
#        "Contents: read and write" scoped to ONLY the 1oom-mp-releases repo.
#        (~/.config/1oom/release_token_ro, the read-only family token, should also exist
#        so build-win.sh bakes it into the bundle's Update.bat.)
#
# It rebuilds the Windows bundle first, so what's published always matches HEAD + working tree
# (pass --no-build to publish an already-built win-bundle, e.g. from CI where the build is a
# separate step).
# Run it from anywhere:  bash engines/fork/publish-release.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO="jacob-t-radford/1oom-mp-releases"
TOK_FILE="$HOME/.config/1oom/release_token_rw"

[ -f "$TOK_FILE" ] || { echo "ERROR: $TOK_FILE not found (fine-grained token, Contents: read+write on $REPO)"; exit 1; }
TOK="$(tr -d ' \t\r\n' < "$TOK_FILE")"

if [ "${1:-}" != "--no-build" ]; then bash "$SCRIPT_DIR/build-win.sh"; fi

SHA="$(git -C "$SCRIPT_DIR" rev-parse --short HEAD)"
TAG="v$(date +%Y.%m.%d-%H%M)-$SHA"
BODY="Windows unzip-and-play bundle (1oom-mp.zip) and the no-data self-update zip that Update.bat fetches (1oom-mp-update.zip). Built from 1oom-mp@$SHA. Everyone must run the same version to play together."

echo "Creating release $TAG on $REPO ..."
RID="$(curl -sf -H "Authorization: Bearer $TOK" -H "Accept: application/vnd.github+json" \
    -X POST "https://api.github.com/repos/$REPO/releases" \
    -d "{\"tag_name\":\"$TAG\",\"name\":\"1oom-mp $TAG\",\"body\":\"$BODY\"}" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')"

upload() {
    echo "Uploading $2 ..."
    curl -sf -H "Authorization: Bearer $TOK" -H "Content-Type: application/zip" \
        --data-binary @"$1" \
        "https://uploads.github.com/repos/$REPO/releases/$RID/assets?name=$2" -o /dev/null
}
upload "$ROOT/win-bundle/1oom-mp.zip" "1oom-mp.zip"
upload "$ROOT/win-bundle/1oom-mp-update.zip" "1oom-mp-update.zip"

echo "Published: https://github.com/$REPO/releases/tag/$TAG"
echo "Windows machines with a token-baked Update.bat will pick this up on their next Update."
