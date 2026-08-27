#!/usr/bin/env bash
# Fast, offline repository hygiene gate. Every parser is provided by the pinned repository or the
# same Python/Node/Bash baseline already required by the host tests; no network install is needed.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

for tool in git bash python3 node ruby; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "repo-lint: required tool is unavailable: $tool" >&2
        exit 2
    }
done

while IFS= read -r -d '' path; do bash -n "$path"; done \
    < <(git ls-files --cached --others --exclude-standard -z -- '*.sh')

python3 - "$root" <<'PY'
import json,pathlib,subprocess,sys,tomllib
root=pathlib.Path(sys.argv[1])
raw=subprocess.check_output([
    "git","-C",str(root),"ls-files","--cached","--others","--exclude-standard","-z","--",
    "*.py","*.json","*.toml",
])
for relative in (value for value in raw.decode("utf-8").split("\0") if value):
    path=root/relative
    text=path.read_text(encoding="utf-8")
    if path.suffix==".py": compile(text,str(path),"exec")
    elif path.suffix==".json": json.loads(text)
    elif path.suffix==".toml": tomllib.loads(text)
print("repo-lint: Python, JSON and TOML syntax PASS")
PY

while IFS= read -r -d '' path; do node --check "$path" >/dev/null; done \
    < <(git ls-files --cached --others --exclude-standard -z -- '*.js' '*.mjs')

while IFS= read -r -d '' path; do ruby -c "$path" >/dev/null; done \
    < <(git ls-files --cached --others --exclude-standard -z -- '*.rb')

yaml_files=()
while IFS= read -r -d '' path; do yaml_files+=("$path"); done \
    < <(git ls-files --cached --others --exclude-standard -z -- '*.yml' '*.yaml')
ruby scripts/check-yaml-syntax.rb --self-test "${yaml_files[@]}"
echo "repo-lint: YAML parsed by offline Psych; GitHub semantics use mutation-tested policy (no actionlint download)"

python3 scripts/check-workflow-policy.py --self-test
python3 scripts/check-markdown-links.py --self-test
python3 scripts/check-logic-test-ownership.py --self-test
python3 scripts/check-nvs-contract.py --self-test
python3 scripts/check-host-gate-contract.py --self-test

git diff --check
git diff --cached --check
git show --check --format= HEAD >/dev/null

echo "repo-lint: PASS (shell, Python, Node, JSON, TOML, workflows, links, ownership, diff)"
