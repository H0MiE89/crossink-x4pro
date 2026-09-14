---
description: >
  Triage newly opened CrossInk issues, detect duplicates, and classify
  product impact and reproduction effort.

on:
  issues:
    types: [opened]

permissions:
  contents: read
  issues: read

engine:
  id: codex
  model: gpt-5.6-luna
  args:
    - "-c"
    - 'model_reasoning_effort="xhigh"'

tools:
  bash: false
  cli-proxy: false
  github:
    toolsets: [issues, search, repos]
    min-integrity: none

safe-outputs:
  update-project:
    project: https://github.com/users/uxjulia/projects/3
    github-token: ${{ secrets.GH_AW_WRITE_PROJECT_TOKEN }}
    max: 1

  add-labels:
    allowed: [duplicate]
    target: triggering
    max: 1
    create-if-missing: true

timeout-minutes: 10
---

# CrossInk Issue Triage

Triage issue #${{ github.event.issue.number }}.

First search existing CrossInk issues for potential duplicates.
Then evaluate the issue using the Product Impact, Repro LOE, Readiness,
Type, and Lane rubric below.

The purpose of this workflow is to act as an intake buffer for the project
maintainer. Evaluate the report from both a Product Owner perspective
(product/user impact) and a Scrum/engineering perspective
(reproduction/investigation effort).

Update this GitHub Project:

https://github.com/users/uxjulia/projects/3

Add the triggering issue to the project and populate these fields:

- Impact
- Repro LOE
- Readiness
- Component
- Lane
- Triage Summary

Do not close the issue.
Do not modify the issue body.
Do not assign users.
Do not treat reproduction difficulty as product impact.

## Duplicate Detection

Before classifying the issue, search existing CrossInk issues for reports
describing the same underlying problem.

Search both:

- open issues
- closed issues

Exclude the triggering issue itself.

Consider:

- issue title
- described behavior
- reproduction steps
- affected feature
- device
- firmware version
- EPUB/file-specific conditions
- crash/error symptoms
- screenshots or logs when available

### Confirmed Duplicate

Classify an issue as a duplicate only when there is strong evidence that
the new report describes the same underlying problem as an existing issue.

A matching symptom by itself is not enough.

For example:

- Two reports of the reader crashing when changing font size under the same
  conditions are likely duplicates.
- Two reports that merely say "reader crashed" are not enough to establish
  a duplicate.
- Similar EPUB failures involving different parsing behavior should not
  automatically be considered duplicates.
- A newer report containing substantially different reproduction conditions
  may indicate a related but distinct defect.

When a confirmed duplicate is found:

1. Select the single best canonical issue.
2. Add the `duplicate` label to the triggering issue.
3. Set Lane to `Duplicate`.
4. Include the canonical issue number and title in Triage Summary.
5. Explain briefly why the reports appear to describe the same defect.
6. Do not close the triggering issue.
7. Do not modify the canonical issue.

Use this format in Triage Summary:

Duplicate of #123 — [canonical issue title].

[One or two sentences explaining the matching behavior and reproduction
conditions.]

### Possible / Related Issue

If another issue appears related but there is insufficient evidence that it
is the same underlying defect:

- Do NOT add the `duplicate` label.
- Do NOT set Lane to Duplicate.
- Triage the issue normally.
- Mention the potentially related issue in Triage Summary.

Example:

Related: #123 may involve the same reader rendering path, but the reproduction
conditions differ enough that this should remain a separate issue.

### No Duplicate Found

If no sufficiently similar issue exists, continue normal triage without
mentioning duplicate detection.

## Impact

### High

The issue materially prevents normal firmware functionality.

Examples:

- firmware crash
- reboot or boot failure
- feature is unusable
- normal reader functionality is blocked
- data loss or corruption
- major regression in previously working behavior
- common workflow cannot be completed

A High-impact issue remains High even if it is difficult to reproduce.

### Medium

The issue affects functionality but appears limited to a particular
user, book, configuration, input, or edge case.

Examples:

- EPUB-specific failure
- unusual or malformed EPUB
- unoptimized EPUB causing a crash
- uncommon configuration
- device-specific edge case
- behavior affecting a limited subset of users

### Low

The issue does not materially prevent firmware functionality.

Examples:

- UI polish
- UX inconvenience
- cosmetic display issue
- translation issue
- minor quality-of-life bug
- documentation misunderstanding
- clear user error

## Repro LOE

This measures effort required to reproduce or investigate the problem,
not effort required to implement the eventual fix.

### Low

Use Low when reproduction should be straightforward.

Examples:

- deterministic reproduction steps
- clearly written issue
- useful logs provided
- required EPUB or test file provided
- reproduces consistently
- UI/UX issue reproducible in the simulator
- no special hardware or environment required

### Medium

Use Medium when some investigation or setup is necessary.

Examples:

- reproduction steps appear reasonable but require additional setup
- device-dependent but common hardware is sufficient
- EPUB-specific issue with sample provided
- behavior is somewhat intermittent
- several variables must be tested

### High

Use High when reproduction is uncertain or expensive.

Examples:

- vague or incomplete reproduction steps
- intermittent behavior
- special hardware required
- display-specific or hardware-specific problem
- EPUB-specific failure without the affected EPUB
- reporter-specific environment
- insufficient logs or diagnostic information
- issue cannot reasonably be reproduced in the simulator

## Readiness

Set exactly one:

- Ready
- Needs Info
- Needs Sample
- Needs Hardware

### Ready

There is enough information to begin reproducing or investigating the issue.

### Needs Info

Important reproduction information is missing or ambiguous.

### Needs Sample

The issue appears dependent on a file such as an EPUB or XTC and the
necessary sample has not been supplied.

### Needs Hardware

Investigation requires hardware or a hardware/display combination that
cannot reasonably be reproduced through the simulator.

## Component

Set exactly one:

- Firmware
- EPUB
- Hardware
- UX
- Translation
- Support

Use Support for reports that are primarily user error, configuration,
usage questions, or expected behavior rather than a firmware defect.

## Lane

Determine Lane from Impact, Repro LOE, Readiness, and Component.

### Fast Track

High impact + Low Repro LOE + Ready.

These are the highest-value easy wins.

### Investigate

High-impact problems that require Medium or High Repro LOE,
additional evidence, hardware, or investigation.

Do not demote serious bugs simply because investigation is difficult.

### Quick Win

Medium or Low impact + Low Repro LOE + Ready where the change would
provide meaningful user value.

Simulator-reproducible UX bugs are good candidates.

### Backlog

Medium impact + Medium Repro LOE and sufficiently actionable.

### Later

Medium impact + High Repro LOE, or Low impact + Medium Repro LOE.

### Icebox

Low impact + High Repro LOE, obvious support/user-error reports,
or work whose cost is clearly disproportionate to user impact.

### Duplicate

Use only when the issue is a confirmed duplicate of an existing issue.

The canonical issue must be identified explicitly in Triage Summary.

Do not use Duplicate merely because another issue is similar or related.

## Triage Summary

Write 2-4 concise sentences for the maintainer.

Include:

1. Why the Impact classification was selected.
2. What makes reproduction easy or difficult.
3. Any important missing information.
4. Why the selected Lane is appropriate.

Do not address the reporter directly.
Write this as an internal Product/Engineering triage note.

## General rules

- Base conclusions only on evidence available in the issue and repository.
- Never invent reproduction results.
- Missing information increases Repro LOE or changes Readiness; it does
  not automatically lower Impact.
- A difficult-to-reproduce crash can still be High Impact.
- A trivial simulator-reproducible UI issue may be Low Impact but Low LOE.
- Prefer conservative classifications when evidence is ambiguous.
- Do not spend significant time debugging the issue during triage.
- This workflow performs intake classification, not root-cause analysis.
