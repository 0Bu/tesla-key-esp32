#!/usr/bin/env bash
# Configure git to use project-managed hooks from .githooks/
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

chmod +x .githooks/pre-commit .githooks/pre-push
git config core.hooksPath .githooks

echo "Git hooks configured successfully: core.hooksPath set to .githooks"
echo "Active hooks:"
echo "  - pre-commit: secrets, partitions.csv, -Os flag, pure-logic includes, web security, host tests"
echo "  - pre-push:   main branch protection, PR policy gates"
