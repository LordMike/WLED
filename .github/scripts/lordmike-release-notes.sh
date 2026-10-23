#!/usr/bin/env bash
# Generate Markdown release notes for a LordMike fork release tag.
#
# Usage: lordmike-release-notes.sh <tag> [previous-tag]
#
# Lists the fork's own commits and the upstream WLED commits pulled in since
# the previous fork release. Needs full history and the upstream branches as
# refs/remotes/upstream/{16_x,main} (see lordmike-release.yml).
#
# Environment:
#   FORK_REPO      GitHub repo of the fork      (default: LordMike/WLED)
#   UPSTREAM_REPO  GitHub repo of upstream WLED (default: wled/WLED)
#   UPSTREAM_REMOTE remote name holding upstream branches (default: upstream)
set -euo pipefail

tag="${1:?usage: $0 <tag> [previous-tag]}"
fork_repo="${FORK_REPO:-LordMike/WLED}"
upstream_repo="${UPSTREAM_REPO:-wled/WLED}"
remote="${UPSTREAM_REMOTE:-upstream}"
upstream_refs=()
for b in 16_x main; do
  git rev-parse -q --verify "refs/remotes/$remote/$b" >/dev/null && upstream_refs+=("$remote/$b")
done
[ ${#upstream_refs[@]} -gt 0 ] || { echo "no upstream refs under $remote/" >&2; exit 1; }
exclude_upstream=("${upstream_refs[@]/#/^}")

# Previous fork release: the fork tag created just before this one.
prev="${2:-}"
if [ -z "$prev" ]; then
  prev=$(git tag -l 'v*lordmike*' --sort=creatordate | grep -xF -B1 "$tag" | grep -vxF "$tag" || true)
fi

# Newest upstream commit contained in a fork commit.
upstream_base() {
  local best="" c
  for r in "${upstream_refs[@]}"; do
    c=$(git merge-base "$1" "$r" 2>/dev/null) || continue
    if [ -z "$best" ] || git merge-base --is-ancestor "$best" "$c"; then best=$c; fi
  done
  echo "$best"
}

# "- [`abc1234`](url) Subject", with upstream "#123" pointed at the right repo
# and @mentions defused so release notes don't ping people.
list_commits() {
  local repo=$1; shift
  git log --format='%H %s' "$@" | while read -r sha subject; do
    subject=$(sed -E "s/(^|[^[:alnum:]/])#([0-9]+)/\1$(sed 's/\//\\\//' <<<"$repo")#\2/g; s/@/@\&#8203;/g" <<<"$subject")
    printf -- '- [`%s`](https://github.com/%s/commit/%s) %s\n' "${sha:0:8}" "$repo" "$sha" "$subject"
  done
}

short() { git rev-parse --short=8 "$1"; }
link() { printf '[`%s`](https://github.com/%s/commit/%s)' "$(short "$2")" "$1" "$(git rev-parse "$2")"; }

new_base=$(upstream_base "$tag")

echo "## Fork changes"
echo
if [ -n "$prev" ]; then
  # --cherry-pick drops fork commits that were only rebased onto a newer upstream.
  fork=$(list_commits "$fork_repo" --cherry-pick --right-only "$prev...$tag" "${exclude_upstream[@]}")
else
  fork=$(list_commits "$fork_repo" "$new_base..$tag" "${exclude_upstream[@]}")
fi
echo "${fork:-No fork changes.}"
echo

echo "## Upstream WLED changes"
echo
prev_base=""
[ -n "$prev" ] && prev_base=$(upstream_base "$prev")
if [ -z "$prev_base" ]; then
  echo "Based on upstream WLED at $(link "$upstream_repo" "$new_base")."
elif [ "$(git rev-parse "$prev_base")" = "$(git rev-parse "$new_base")" ]; then
  echo "None. Still based on upstream WLED at $(link "$upstream_repo" "$new_base")."
else
  count=$(git rev-list --count "$prev_base..$new_base")
  echo "Upstream WLED moved from $(link "$upstream_repo" "$prev_base") to $(link "$upstream_repo" "$new_base") ([compare](https://github.com/$upstream_repo/compare/$(git rev-parse "$prev_base")...$(git rev-parse "$new_base")))."
  echo
  echo "<details><summary>$count upstream commits</summary>"
  echo
  list_commits "$upstream_repo" "$prev_base..$new_base"
  echo
  echo "</details>"
fi

if [ -n "$prev" ]; then
  echo
  echo "**Full changelog:** https://github.com/$fork_repo/compare/$prev...$tag"
fi
