#!/usr/bin/env bash
# Build this worktree's SmartHome sources; restore shared package links on exit.
set -euo pipefail
TASK_REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TASK_ROOT=$TASK_REPO
while [[ $TASK_ROOT != / && ! -f $TASK_ROOT/build.sh ]]; do
  TASK_ROOT=$(dirname "$TASK_ROOT")
done
[[ -f $TASK_ROOT/build.sh && -d $TASK_ROOT/nuttx ]] || {
  echo 'Cannot locate the OpenVela workspace' >&2; exit 1;
}
links=("$TASK_ROOT/packages/demos/smart_home" "$TASK_ROOT/packages/cAGENT" "$TASK_ROOT/packages/cagent_addons")
targets=("$TASK_REPO/demos/smart_home" "$TASK_REPO/packages/cAGENT" "$TASK_REPO/packages/cagent_addons")
originals=()
for link in "${links[@]}"; do
  [[ -L $link ]] || { echo "Expected existing symlink: $link" >&2; exit 1; }
  originals+=("$(readlink "$link")")
done
mkdir -p "$TASK_ROOT/out"
exec 9>"$TASK_ROOT/out/smart_home_worktree_build.lock"
flock -n 9 || { echo 'Another SmartHome worktree build is active' >&2; exit 1; }
restore_links()
{
  for i in "${!links[@]}"; do
    if [[ -L ${links[$i]} && $(readlink "${links[$i]}") == "${targets[$i]}" ]]; then
      ln -sfnT -- "${originals[$i]}" "${links[$i]}"
    fi
  done
}
trap restore_links EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'SmartHome source: %s\n' "$TASK_REPO/demos/smart_home"
printf 'Use this wrapper without any concurrent firmware build.\n'
for i in "${!links[@]}"; do
  ln -sfnT -- "${targets[$i]}" "${links[$i]}"
done
"$TASK_REPO/scripts/build_p4x.sh" smart_home_alarms "$@"
