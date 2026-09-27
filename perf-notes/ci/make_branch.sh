#!/usr/bin/env bash
# Create and push a throwaway perf-ci/<name> branch that runs perf-block.yml.
#
#   perf-notes/ci/make_branch.sh NAME CONFIG.json [WORKFLOW.yml]
# (WORKFLOW defaults to perf-notes/ci/perf-block.yml)
#
# CONFIG.json: {"arms": {...}, "jobs": N, "blocks_per_job": B, "scale": S, "values": V,
#               "bench": "regex", "configure": "..."}; see build_arms.py for arm specs.
# Arm refs may be any local commit-ish; they are resolved to SHAs here.  The first
# arm is the baseline.  The branch tip is one commit whose parents are all arm
# commits (so runners can fetch each arm by SHA) and whose tree is the first
# arm's tree with all built-in workflows removed, plus perf-block.yml and the
# perf-notes tools.  Nothing else runs on the branch.
set -euo pipefail
name=$1 config=$2 workflow=${3:-perf-notes/ci/perf-block.yml}
top=$(git rev-parse --show-toplevel)
cd "$top"
tmp=$(mktemp -d)

# Resolve refs to SHAs and collect parents.
python3 - "$config" "$tmp/config.json" > "$tmp/parents" <<'EOF'
import json, subprocess, sys
c = json.load(open(sys.argv[1]))
parents = []
for k, v in c['arms'].items():
    if isinstance(v, str):
        v = {'ref': v}
    if 'ref' in v:
        v['ref'] = subprocess.check_output(['git', 'rev-parse', v['ref'] + '^{commit}'], text=True).strip()
        if v['ref'] not in parents:
            parents.append(v['ref'])
    c['arms'][k] = v
json.dump(c, open(sys.argv[2], 'w'), indent=1)
print(' '.join(parents))
EOF
read -r -a shas < "$tmp/parents"
first=${shas[0]}
parents=()
for s in "${shas[@]}"; do parents+=(-p "$s"); done

export GIT_INDEX_FILE=$tmp/index
git read-tree "$first"
git ls-files .github | xargs -r git update-index --force-remove
add() { git update-index --add --cacheinfo 100644,"$(git hash-object -w "$1")","$2"; }
add "$workflow" ".github/workflows/$(basename "$workflow")"
for f in perf-notes/tools/*.py perf-notes/ci/*.py; do add "$f" "$f"; done
add perf-notes/ci/loops.json perf-notes/ci/loops.json
add "$tmp/config.json" perf-notes/ci/config.json
tree=$(git write-tree)
unset GIT_INDEX_FILE
commit=$(git commit-tree "$tree" "${parents[@]}" -m "perf-ci: $name

$(cat "$tmp/config.json")")
rm -rf "$tmp"
echo "commit $commit"
git push -f origin "$commit:refs/heads/perf-ci/$name"
