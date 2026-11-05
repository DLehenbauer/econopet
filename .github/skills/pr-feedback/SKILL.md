---
name: pr-feedback
description: Address pull request code review (CR) feedback through independent verification, related-issue sweeps, published fixes, and evidence-backed thread resolution. Repeat review/fix cycles until Copilot reports no actionable findings on the current head. Use when asked to handle PR feedback, fix review findings, or resolve review comments on a PR identified by number, URL, or conversation context.
---

# PR Feedback

Verify findings independently, choose the best disposition, and publish
evidence before resolving threads. A reviewer's diagnosis and proposed fix
are separate claims.

## When to Use

Use for handling feedback on an existing PR, not for requests to edit this skill.

## Prerequisites

Resolve the repository and PR from the latest explicit number/URL or unambiguous
chat context. Clarify genuinely ambiguous targets. Use authenticated `gh` access
and load the [code-review skill](../code-review/SKILL.md) before evaluating code.
Use an isolated worktree at the actual PR head when the workspace contains
another task's changes.

## Workflow

Track these steps in session TODOs. Retain each finding's comment/thread IDs,
evidence, disposition, validation, published commit, and reply/resolution state.
Track independently discovered issues separately from reviewer findings.
Retain the current head SHA, review request/time, processed feedback IDs, and
waiting deadline so resumed runs reconcile rather than duplicate operations.
Persist this ledger under `.cache/ai/pr-feedback/<owner>-<repo>-<pr>/`, outside
the PR's tracked changes. Include finding dispositions, validation, published
commits, reply/resolution IDs, and the next action. Reconcile it with GitHub on
resume. Use one active loop per PR and preserve another loop's in-flight work.

### 1. Establish the PR and Feedback Snapshot

Read the current base/head revisions, diff, reviews, discussion, and inline
threads with replies. `gh pr view --comments` does not enumerate inline threads.
Retrieve `repository.pullRequest.reviewThreads` through `gh api graphql`,
including IDs, resolution/outdated state, paths, and line locations. Paginate
both threads and nested comments.

Check prior replies and current code before repeating completed work.
An outdated location does not establish that a finding is fixed.

### 2. Independently Verify Each Finding

Apply the code-review workflow at the current PR head. Establish the trigger,
contract, consequence, and supporting reproduction, execution trace, or primary
evidence. Assign a disposition independently of the suggested implementation:

| Disposition | Required evidence and action |
| --- | --- |
| Correct, change justified | Implement the best focused fix or improvement. |
| Incorrect | Identify the false premise with evidence, then reply and resolve. |
| Technically correct, retain design | Acknowledge the limitation and justify the current tradeoff, then reply and resolve within the task's authority. |
| Already addressed | Verify the existing change covers the reported case, then reply and resolve. |
| Uncertain or blocked | State the missing evidence, permission, or decision and leave unresolved. |

For retained designs, compare concrete alternatives on relevant usability,
complexity, maintenance, compatibility, performance, and risk. Ground the
decision in requirements and supported-use boundaries. An undesirable fix does
not make the observation false. Do not dismiss requirement breaches, data loss,
or security defects as tradeoffs. Escalate decisions that change product policy
or exceed the task's authority.

### 3. Implement and Validate Justified Fixes

For every confirmed defect or improvement, automatically sweep beyond the cited
lines for the same mechanism, invariant, or rationale. Inspect equivalent entry
points, sibling implementations, callers, lifecycle/error paths, and repeated
patterns in the PR and directly coupled code.

Independently verify and fix related issues the reviewer missed. Apply the same
disposition and tradeoff criteria to each, rather than copying the original fix
mechanically. Stop at unrelated cleanup and report unrelated pre-existing
issues separately.

Choose the simplest justified implementation, not necessarily the reviewer's
proposal. Add focused regressions for original and independently discovered
fixes, demonstrating failure before and success after. Run repository-required
checks and disclose validation gaps.

Commit and push to the PR's actual head repository/branch (including forks).
Recheck the remote head before publication. Integrate and revalidate concurrent
changes rather than overwriting them. Do not amend or force-push without
explicit authorization.

### 4. Reply With Evidence and Resolve Addressed Threads

Reply in the original thread with the disposition, evidence/code references,
published commit and validation (if changed), and why the chosen fix or retained
design is preferable. Include the related sweep's scope and outcome without
claiming a broader audit.

Use the thread's GraphQL node ID with `addPullRequestReviewThreadReply` and
`resolveReviewThread`, or the supported REST reply endpoint followed by the
GraphQL resolution mutation. Thread node IDs and numeric comment IDs are not
interchangeable. Verify publication and post evidence before resolving.
General comments/review summaries may only support a reply, not resolution.
Leave uncertain or blocked findings unresolved.

### 5. Repeat Until the Current Head Passes Copilot Review

After publishing fixes and responses, keep the task open and wait for Copilot
code review. Match the reviewer by its GitHub App/bot identity, not display name.
Record the remote head SHA and use an automatic review already queued for that
head. Otherwise request review or re-review through a supported GitHub mechanism.
Verify that the request was accepted. Do not assume a push or mention starts a
review, and do not duplicate an outstanding request for the same head.

Prefer a supported one-shot wake-up or review-completion event when exposed by
the current toolset. Verify scheduler/hook capabilities before using them:
slash commands such as `/every` or `/at` are not necessarily agent-callable tools.
Do not install repository-wide hooks merely to wait for this PR. Record and
cancel only this loop's timer/subscription when it finishes or changes heads.
GitHub.com's Agent Merge ("Watch and fix") watches Actions failures and merge
conflicts, not CR feedback. It can use published repository skills for its tasks,
but is not this review loop's wake-up mechanism. The Copilot app's Agent Merge
also addresses reviews but aims to merge, which this skill does not authorize.
Local untracked skills and ledgers are not automatically available to cloud runs.

Otherwise check reviews, review summaries, inline threads/replies, and relevant
check runs at 60-second intervals, honoring rate limits. Wait up to 30 minutes
per review, preserving the deadline across wake-ups rather than restarting it.
Keep waiting work attached unless a supported persistent scheduler owns resumption.
A submitted review may use `COMMENTED` even when clean. Neither that state alone,
a green check, resolved old threads, nor silence establishes satisfaction.

Return to steps 1-4 for new or reopened findings, including findings present only
in the review summary. Keep independent verification and related-issue sweeps:
do not make an unjustified change merely to appease the bot. If Copilot repeats
a finding after an evidence-backed rejection or retained-design response and
offers no new evidence, report the disagreement as blocked rather than cycling
the same patch, reply, or review request.

Complete successfully only after a submitted Copilot review explicitly reports
no outstanding actionable findings on the exact current remote head, all earlier
findings have published dispositions, and no unresolved finding or newer feedback
remains. Recheck the head, feedback, replies, resolutions, and CI immediately
before reporting. Any head change invalidates an older clean review and requires
another cycle. Do not merge the PR or dismiss reviews.

If review cannot be requested, fails, exceeds the waiting deadline, or needs a
decision outside the task's authority, report the specific blocker and retain
the pending head/request/feedback state for resumption. Do not call that approval
or claim the feedback loop completed.

Report the PR link, fixes/commits, no-change decisions, validation (including
pending/failed CI), the final Copilot review URL and reviewed SHA, or the waiting
blocker. Distinguish reviewer findings from independently discovered issues.
