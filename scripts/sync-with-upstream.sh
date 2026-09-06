#!/usr/bin/env bash
# Merge canonical MetaversalCorp into local main without losing fork-only commits.
#
# Remotes (expected):
#   origin  → MetaversalCorp/Sneeze (canonical, read + PR target)
#   alfa    → AlfaOmegaGrafx/Sneeze (your fork, push target)
#
# When the fork is several commits ahead and canonical has moved, a plain
# `git pull --ff-only origin main` fails. This script merges origin/main instead,
# runs continuity checks, and optionally pushes to alfa.
#
# Usage:
#   ./scripts/sync-with-upstream.sh              # merge canonical, verify, stay local
#   ./scripts/sync-with-upstream.sh --push-fork  # merge, verify, push alfa main
#   ./scripts/sync-with-upstream.sh --dry-run    # show ahead/behind only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SNEEZE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SNEEZE_DIR"

CANONICAL_REMOTE="${CANONICAL_REMOTE:-origin}"
FORK_REMOTE="${FORK_REMOTE:-alfa}"
BRANCH="${BRANCH:-main}"
PUSH_FORK=0
DRY_RUN=0
SKIP_BUILD=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --push-fork) PUSH_FORK=1 ;;
    --dry-run)   DRY_RUN=1 ;;
    --no-build)  SKIP_BUILD=1 ;;
    -h|--help)
      sed -n '2,16p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown flag: $1" >&2
      exit 1
      ;;
  esac
  shift
done

ensure_remote() {
  local name="$1" url="$2"
  if git remote get-url "$name" &>/dev/null; then
    return 0
  fi
  echo "Adding remote $name → $url"
  git remote add "$name" "$url"
}

ensure_remote "$CANONICAL_REMOTE" "git@github.com:MetaversalCorp/Sneeze.git"
ensure_remote "$FORK_REMOTE" "git@github.com:AlfaOmegaGrafx/Sneeze.git"

echo "Fetching $CANONICAL_REMOTE and $FORK_REMOTE..."
git fetch "$CANONICAL_REMOTE" "$BRANCH"
git fetch "$FORK_REMOTE" "$BRANCH" 2>/dev/null || true

git checkout "$BRANCH"

LOCAL="$(git rev-parse HEAD)"
UPSTREAM="$CANONICAL_REMOTE/$BRANCH"
UPSTREAM_SHA="$(git rev-parse "$UPSTREAM")"

read -r AHEAD BEHIND < <(git rev-list --left-right --count "$LOCAL...$UPSTREAM")

echo ""
echo "Branch: $BRANCH @ $(git rev-parse --short HEAD)"
echo "  vs $UPSTREAM ($(git rev-parse --short "$UPSTREAM_SHA"))"
echo "  ahead of canonical:  $AHEAD"
echo "  behind canonical:    $BEHIND"

if [[ "$AHEAD" -eq 0 && "$BEHIND" -eq 0 ]]; then
  echo "Already aligned with canonical."
elif [[ "$AHEAD" -eq 0 && "$BEHIND" -gt 0 ]]; then
  echo "Fast-forwarding to canonical..."
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] would: git merge --ff-only $UPSTREAM"
    exit 0
  fi
  git merge --ff-only "$UPSTREAM"
elif [[ "$BEHIND" -gt 0 ]]; then
  echo "Merging canonical into fork branch (preserves your $AHEAD local commit(s))..."
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] would: git merge $UPSTREAM -m 'Merge MetaversalCorp/main into fork branch'"
    exit 0
  fi
  git merge "$UPSTREAM" -m "Merge MetaversalCorp/main into fork branch"
else
  echo "Ahead of canonical only — nothing to merge from upstream."
fi

continuity_checks() {
  echo ""
  echo "Continuity checks..."

  local failed=0
  local f
  # Map_Object.h was removed upstream in favor of MapSvc (Aug 2026).
  for f in \
    scripts/build-linux.sh \
    scripts/build-deps.sh \
    include/Sneeze.h \
    src/context/scene/MapSvc.h \
    src/CMakeLists.txt; do
    if [[ ! -e "$f" ]]; then
      echo "  FAIL missing: $f"
      failed=1
    fi
  done

  if ! bash -n scripts/build-linux.sh || ! bash -n scripts/build-deps.sh; then
    echo "  FAIL bash -n on build scripts"
    failed=1
  fi

  if [[ -x scripts/build-dgx-spark.sh ]] && ! bash -n scripts/build-dgx-spark.sh; then
    echo "  FAIL bash -n on build-dgx-spark.sh"
    failed=1
  fi

  if [[ -f builds/linux-arm64/install/release/lib/libSneeze.a && "$SKIP_BUILD" -eq 0 ]]; then
    echo "  Incremental Sneeze rebuild (deps unchanged)..."
    if ! bash ./scripts/build-linux.sh; then
      echo "  FAIL incremental build-linux.sh"
      failed=1
    elif [[ -x builds/linux-arm64/install/release/bin/SneezeTest ]]; then
      echo "  Smoke: SneezeTest --wasm --net"
      if ! builds/linux-arm64/install/release/bin/SneezeTest --wasm --net; then
        echo "  FAIL SneezeTest smoke"
        failed=1
      fi
    fi
  else
    echo "  Skipping compile smoke (no libSneeze.a or --no-build)."
  fi

  if [[ "$failed" -ne 0 ]]; then
    echo ""
    echo "Continuity checks FAILED — fix before pushing to $FORK_REMOTE."
    exit 1
  fi
  echo "Continuity checks OK."
}

if [[ "$DRY_RUN" -eq 1 ]]; then
  exit 0
fi

if [[ "$BEHIND" -gt 0 || "$AHEAD" -gt 0 ]]; then
  continuity_checks
fi

if [[ "$PUSH_FORK" -eq 1 ]]; then
  echo ""
  echo "Pushing $BRANCH → $FORK_REMOTE..."
  git push "$FORK_REMOTE" "$BRANCH"
  echo "Fork updated. Open PR: MetaversalCorp/Sneeze ← AlfaOmegaGrafx/Sneeze ($BRANCH)"
fi
