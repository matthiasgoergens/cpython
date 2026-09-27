#!/usr/bin/env bash
# Create and push a throwaway perf-ci/<name> branch that runs perf-block.yml.
#
#   perf-notes/ci/make_branch.sh NAME JOBS BLOCKS_PER_JOB SCALE BENCH_REGEX arm1=REF arm2=REF ...
#
# The first arm is the baseline.  The branch tip is one commit whose parents
# are all arm commits (so the runners can fetch each arm by SHA) and whose
# tree is the first arm's tree with all built-in workflows removed, plus
# perf-block.yml and the perf-notes tools.  Nothing else runs on the branch.
set -euo pipefail
name=$1 jobs=$2 blocks=$3 scale=$4 bench=$5; shift 5
top=$(git rev-parse --show-toplevel)
cd "$top"

arms_json="{"
parents=()
first=
for spec in "$@"; do
  arm=${spec%%=*}; sha=$(git rev-parse "${spec#*=}^{commit}")
  [ -z "$first" ] && first=$sha
  arms_json+="\"$arm\": \"$sha\","
  parents+=(-p "$sha")
done
arms_json="${arms_json%,}}"

tmp=$(mktemp -d)
export GIT_INDEX_FILE=$tmp/index
git read-tree "$first"
git ls-files .github/workflows | grep -v 'posix-deps-apt.sh$' | xargs -r git rm -q --cached
cat > "$tmp/config.json" <<EOF
{"arms": $arms_json, "jobs": $jobs, "blocks_per_job": $blocks, "scale": $scale, "values": 3,
 "bench": "$bench", "configure": "--enable-optimizations --with-lto"}
EOF
add() { git update-index --add --cacheinfo 100644,"$(git hash-object -w "$1")","$2"; }
add perf-notes/ci/perf-block.yml .github/workflows/perf-block.yml
for f in perf-notes/tools/*.py; do add "$f" "$f"; done
add perf-notes/ci/loops.json perf-notes/ci/loops.json
add "$tmp/config.json" perf-notes/ci/config.json
tree=$(git write-tree)
unset GIT_INDEX_FILE
commit=$(git commit-tree "$tree" "${parents[@]}" -m "perf-ci: $name

$(cat "$tmp/config.json")")
rm -rf "$tmp"
echo "commit $commit"
git push -f origin "$commit:refs/heads/perf-ci/$name"
