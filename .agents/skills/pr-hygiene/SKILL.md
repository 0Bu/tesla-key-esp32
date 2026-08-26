---
name: pr-hygiene
description: Read-only screen of this PR's commits, title/body and touched documentation for personal or private information (LAN IP addresses, MAC addresses, vehicle VINs, WiFi network names, hostnames, phone numbers, emails, private session/transcript links) and for content not written in English. Reports findings and the exact gate record only; never edits a commit, a file or a PR body, and never commits, pushes, merges, releases, flashes, OTAs, or contacts a live device/vehicle unless the user separately authorizes implementation.
---

> **Canonical runner-neutral skill.** Read [`AGENTS.md`](../../../AGENTS.md) before acting.
> Project skills are canonical under [`.agents/skills/`](../), and lifecycle/PR policy is
> enforced by the runner-neutral core under [`tools/agent-hooks/`](../../../tools/agent-hooks/).
> This skill does not grant permissions beyond the user's explicit request.
> Invoke this workflow canonically as `$pr-hygiene`.

# pr-hygiene — screen commits, PRs and docs for private data and non-English text

`$skill-audit` and `$project-review` audit whether code, config and docs **agree with each
other**. Neither looks at whether the PR's own words — its title, body, commit messages, and any
documentation prose it touches — are safe to publish. This project's own history has produced
both failure modes: a PR body quoting the tester's real LAN IP, MAC addresses and a real Tesla
VIN as "example" values, and a run of early PR bodies written entirely in German. `$pr-hygiene`
is the dedicated, unconditional gate for exactly those two concerns, checked at PR creation,
every push, and merge — never assume a clean `$skill-audit` or `$project-review` also covers it;
their scope is coherence, not confidentiality or language.

## What counts as a finding

### `PRIVACY-LEAK` — personal or private information

- A full or partial private-use IP address standing in for a real device (including a bare
  last-octet shorthand used for "the board on the bench"). Documentation examples must use
  `<ESP32-IP>`, the IPv4 documentation ranges `192.0.2.0/24`, `198.51.100.0/24` or
  `203.0.113.0/24`, or the IPv6 documentation prefix `2001:db8::/32`; do not guess that an
  arbitrary RFC1918 address is generic. Universal addresses such as `0.0.0.0`, `127.0.0.1`,
  the device's fixed setup address, and loopback proxy URLs are not findings.
- A real device MAC address — the vehicle's own, a nearby BLE peer's, or a network interface's
  (Ethernet/W5500, WiFi AP/mesh node) — even when only its OUI and suffix are shown. A generic,
  locally administered placeholder (`aa:bb:cc:dd:ee:ff`) is not a finding.
- A real Tesla VIN (17 chars, `[A-HJ-NPR-Z0-9]`) used as an "example" instead of this project's
  own `<VIN>` placeholder convention.
- A real WiFi network/mesh SSID, router/NAS hostname, or syslog/MQTT broker address that
  identifies the reporter's home network, instead of a generic placeholder (`MyWiFi`, plain
  prose like "the test device") or a documentation-reserved value.
- A personal email address, phone number or physical address. A well-known convention is not a
  finding on its own — `git@github.com` in an SSH remote (`git@github.com:owner/repo`) and a
  service-generated noreply commit address are standard publication metadata, not leaks.
- A private session/transcript URL, non-public ticket or internal dashboard link, or a URL carrying
  an authentication token. A public documentation or source permalink is not a finding.
- The board's own diagnostic MAC (this firmware's `sys.board_mac`) is intentionally visible
  on-device (see `docs/README.md` `/status?redact=1` and `main/logic/status_model.hpp`); quoting
  that design choice is not itself a finding. Only a value that is plausibly the tester's own
  real hardware or network, written into PR/commit prose, is.

### `LANGUAGE` — content not written in English

- A PR title or PR-body paragraph written wholly or partly in a language other than English.
- A commit subject or body not written in English.
- Non-English prose in a touched Markdown/doc file. A sentence that quotes the literal historical
  string a piece of code or UI produced in another language (for example, a PR describing what an
  old localized display label actually rendered on screen) is not itself a finding — the
  surrounding descriptive prose must still be English.

## How to run the audit

Work in this order — a single read-only pass: enumerate → check → report → stop.

1. **Enumerate the exact content in scope for the current PR/commit range**: the PR title and
   body (or, before a PR exists, the body about to be submitted), every commit message in
   `<merge-base>..HEAD`, and the prose of every touched `*.md` file (plus any other doc format the
   diff adds). Also skim the diff of touched non-doc files for an obviously real IP/MAC/VIN/SSID
   hardcoded as data or in a comment — the same leak is just as real inside source as inside a PR
   body.
2. **Classify each piece of text** against the two finding categories above. Do not flag a
   generic/placeholder value, a universal address, or a well-known syntax convention. A quoted
   historical fact may explain non-English UI text, but it never makes a real private value safe
   to republish.
3. **Verify before asserting.** A partial IP/MAC needs enough surrounding context to tell a real
   device reference from an unrelated decimal (a version string, a percentage, a numbered list).
   A language flag needs an actual non-English sentence, not a single loanword or a proper noun.
4. **Report, do not correct.** Every finding gets its exact location and the minimal fix (a
   generic replacement value, or a translation) — proposed, not applied, during an audit-only run.
5. **Report gate readiness without editing anything.** If and only if nothing is found, provide
   the exact record a separately authorized PR-body update would need:

   ```
   - [x] `$pr-hygiene` clean — content gate @ <short-sha>
   ```

### Termination — one report-only pass

A `$pr-hygiene` invocation reads, checks, reports, and stops. It does not invoke itself, edit a
finding, or re-audit an edit. A separately authorized implementation may address accepted
findings (redact the value, translate the prose); an independent later run verifies the result
against the new head.

## The PR gate

[`tools/agent-hooks/require-pr-gates.sh`](../../../tools/agent-hooks/require-pr-gates.sh), invoked
by the project hook configuration, refuses to open a PR, push to one, or merge one until its
`$pr-hygiene` record is uniquely present and stamped with the exact commit being published — the **strictest**
of the PR gates, since it fires at every one of those checkpoints rather than only create/push
(`$skill-audit`) or only merge (`$project-review`, conditionally `$feature-docs`). There is no
file marker; pass state lives only in the PR body and is parsed by the neutral core, using the
same CommonMark task-list record mechanism as the other gates (see `$skill-audit`'s *The PR gate*
section for the exact recognized/rejected command shapes — duplicate, dynamic, hidden, fenced,
quoted-example, HTML, blockquote, stale-head and ambiguous records fail closed there exactly as
they do here).

For a new PR, a separately authorized publisher supplies the record in the one exact body source;
for an existing PR, a separately authorized PR-body edit precedes push; for a merge, the record
must still match the exact head being merged. Any new commit stales the record, at every
checkpoint. `$pr-hygiene` is **not** a subset of `$project-review` or `$skill-audit` and neither of
those establishes its readiness — confidentiality and language are a separate axis from
coherence, so a clean full review does not imply a clean `$pr-hygiene` run.

## Report structure

```
# PR hygiene — tesla-key-esp32 (<date>)

## Summary
<1–3 sentences: what was checked (commit range, PR title/body, touched docs) and how many findings.>

## Findings
For each, in priority order:
### [PRIVACY-LEAK|LANGUAGE] <short title>
- **Where:** PR title | PR body | commit `<short-sha>` | `path:line`
- **What:** the exact leaking value or non-English text (redact the value itself in the report if
  quoting it verbatim would repeat the leak)
- **Proposed fix:** the exact generic replacement or translation; not applied during this audit

## Coverage
<PR title/body, each commit message, each touched doc: ✓ clean / ✗ finding above.>

## Gate
<ready record for a separately authorized PR-body update | withheld — findings still open>
```

A clean run is a valid outcome: PR title/body, every commit message and every touched doc ✓,
zero edits, gate record ready but not inserted.
