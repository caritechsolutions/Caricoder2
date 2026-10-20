#!/usr/bin/env bash
#
# caricoder3 - models to try against the technician's own (caricoder3 tech-eval)
# Copyright (c) 2026 CariTech Solutions
#
#   sudo bash /opt/caricoder3/scripts/tech-eval-models.sh            # the candidates below
#   sudo bash /opt/caricoder3/scripts/tech-eval-models.sh REPO ...   # others (Hugging Face repos)
#
# Each is fetched once into /var/lib/caricoder3/models/eval: the repo's
# 4-bit Q4_K_M file, found from the repo's own file list, which also gives
# its SHA-256; the download is checked against it before it is kept. Nothing
# else on the machine changes, and the running technician keeps its model.
# Delete a file there to remove a candidate.
#
set -uo pipefail

DIR=/var/lib/caricoder3/models/eval
REPOS=("$@")
[ ${#REPOS[@]} -gt 0 ] || REPOS=(unsloth/Qwen3.8-27B-GGUF unsloth/Qwen3.6-35B-A3B-GGUF)

say() { printf '%s %s\n' "$(date '+%F %T')" "$*" >&2; }

command -v curl >/dev/null 2>&1 || { say "curl is needed"; exit 1; }
command -v python3 >/dev/null 2>&1 || { say "python3 is needed"; exit 1; }
mkdir -p "$DIR"

for repo in "${REPOS[@]}"; do
    say "$repo: looking for its Q4_K_M file"
    list="$(curl -fsSL --retry 3 "https://huggingface.co/api/models/$repo/tree/main")" ||
        { say "$repo: could not read its file list (is the name right?)"; continue; }
    # one file, not split into parts, not the 'UD' variants: name, size, sha256
    pick="$(printf '%s' "$list" | python3 -c '
import json, sys
files = [f for f in json.load(sys.stdin) if f.get("type") == "file"]
c = [f for f in files if f["path"].endswith("Q4_K_M.gguf") and "/" not in f["path"]
     and "-of-" not in f["path"] and not f["path"].startswith("UD-") and "-UD-" not in f["path"]]
if not c:
    sys.exit(1)
f = c[0]
print(f["path"], f.get("lfs", {}).get("size", f.get("size", 0)), f.get("lfs", {}).get("oid", ""))
')" || { say "$repo: no single Q4_K_M .gguf in it; skipped"; continue; }
    read -r name size sha <<<"$pick"
    dest="$DIR/$name"
    if [ -f "$dest" ]; then
        say "$repo: $name is here already"
        continue
    fi
    [ ${#sha} -eq 64 ] || { say "$repo: the file list gives no SHA-256 for $name; not downloaded"; continue; }
    need_kb=$(( size / 1024 + 2 * 1024 * 1024 ))
    free_kb=$(df -Pk "$DIR" | awk 'NR == 2 { print $4 }')
    if [ "${free_kb:-0}" -lt "$need_kb" ]; then
        say "$repo: $name needs $(( size / 1073741824 + 2 )) GB free in $DIR, $(( free_kb / 1048576 )) GB is; skipped"
        continue
    fi
    say "$repo: downloading $name ($(( size / 1048576 )) MB)"
    if ! curl -fL --retry 3 -C - -o "$dest.part" "https://huggingface.co/$repo/resolve/main/$name"; then
        say "$repo: the download failed (run again to carry on where it stopped)"
        continue
    fi
    say "$repo: checking it"
    got="$(sha256sum "$dest.part" | awk '{ print $1 }')"
    if [ "$got" != "$sha" ]; then
        say "$repo: WRONG checksum ($got, the repo says $sha): deleted"
        rm -f "$dest.part"
        continue
    fi
    mv -f "$dest.part" "$dest"
    say "$repo: ready: $dest"
done
say "models to try: $(ls "$DIR"/*.gguf 2>/dev/null | wc -l) in $DIR; then: caricoder3 tech-eval"
