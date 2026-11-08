# Plan: Introduce Zith--

Objective: introduce the current Zith-- project accurately, give interested
systems programmers a tested way to try it, and collect useful feedback without
presenting Zith design proposals as working Zith-- features.

Preconditions:

- Work from `/home/diogo/Zith` for compiler and documentation checks.
- The compiler in this repository targets Zith--. Zith is a sibling language
  with a separate proposal and is not a future stage or superset of Zith--.
- Finish the author's pending compiler corrections and extensions before
  publishing. This plan sets no calendar launch date. Publication is gated by
  the checks below.
- Treat `/home/diogo/Zith/docs/impl-status.md` as the authority for current
  compiler support. Refresh it only after checking actual compiler behavior.
- The website source repository is `/home/diogo/Zith-website`. It is outside
  the writable root for this task. No website files are changed while saving
  this plan and its announcement draft.
- Before a future task edits the website, obtain write access specifically to
  `/home/diogo/Zith-website`. Do not work around the current filesystem
  boundary.
- The website's About feed uses dated HTML files under
  `/home/diogo/Zith-website/about/posts/` and registers them in
  `/home/diogo/Zith-website/about/posts/index.json`.
- The current `/home/diogo/Zith/README.md` and other project files already have
  user changes. Preserve them. Re-read them before any future edits and do not
  reset, check out, or revert unrelated work.
- The saved source copy is
  `/home/diogo/Zith/docs/plans/zith-minusminus-introduction-draft.md`.
- Files created now:
  `/home/diogo/Zith/docs/plans/zith-minusminus-introduction-draft.md` and
  `/home/diogo/Zith/docs/plans/zith-minusminus-publicity.md`.
- Future website post path: on the day publication is approved, run
  `date +%F` in the `Europe/Lisbon` timezone and use that exact date in
  `/home/diogo/Zith-website/about/posts/<date>-introducing-zith-minusminus.html`.

Do not implement language features, change language contracts, publish
automatically, request coordinated votes, or claim unverified compiler
capabilities.

## Step 1 - Pass the readiness gate

Goal: the compiler, specification, implementation status, and linked public
documentation describe the same current Zith-- behavior.

Sub-steps:

1. Finish the corrections and extensions the author wants before promotion.
2. Re-audit each finding in
   `/home/diogo/Zith/docs/review-website-inconsistencies-2026-11-02.md`
   against the current compiler, specification, implementation status, and
   website source. That review is dated and may be stale. Resolve each finding
   or record why it no longer applies. Do not copy an old fix without checking
   the current files.
3. Check every statement planned for public use against the current compiler.
   Update `docs/impl-status.md` only when a compiler behavior was actually
   rechecked.
4. Build the compiler from the repository root.
5. Run the complete CTest suite.
6. Run the documented example smoke check from the repository root.

Commands:

    agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build -j4
    agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build --output-on-failure
    agent-run -t 30 -c 50 -- ./build/zithc check examples/loops-simple.zith

Expected output:

- The build exits with code `0`.
- CTest reports `100% tests passed`.
- The example check exits with code `0`.
- No unresolved public-facing factual discrepancy remains in the dated website
  review.

Failure checks:

- If the build or tests fail, stop promotion and fix the compiler or tests
  first. Re-run all three commands after the fix.
- If the example check fails, stop and inspect the diagnostic. Do not replace
  the example in public copy until a different example passes the same check.
- If an item in the dated website review cannot be confirmed, leave it open
  and do not publish a page that repeats the disputed claim.

Success criteria: all commands exit with code `0`, and every known
documentation inconsistency that affects public claims is resolved or explicitly
excluded from the announcement.

## Step 2 - Verify the try-it path

Goal: a reader can follow one documented route and successfully check or run a
Zith-- example.

Sub-steps:

1. Use `examples/loops-simple.zith` as the initial example because the
   repository already identifies it as a compiler smoke test.
2. Run it through the local compiler without cache.
3. Inspect the website playground and verify that it uses a compiler build
   compatible with the current Zith-- specification and example.
4. Keep the website playground out of the announcement unless that parity is
   demonstrated. The existence of a playground page alone does not prove that
   it runs current Zith--.
5. Put the verified local command or verified playground link in the final
   website post.

Commands:

    agent-run -t 30 -c 50 -- ./build/zithc --no-cache run examples/loops-simple.zith
    agent-run -t 30 -c 50 -- git -C /home/diogo/Zith-website status --short

Expected output:

- The example exits with code `0`.
- Website status is recorded before any future site edit. Existing changes are
  preserved.
- The final announcement names only the route that was tested.

Failure checks:

- If the example does not exit with code `0`, stop and choose no replacement
  until another checked-in example has passed both `zithc check` and `zithc run`.
- If the website playground cannot be shown to run current Zith--, omit it as a
  try-it path. Keep the verified local command instead.
- If the website repository has existing worktree changes, preserve them and
  isolate the publication change from those files.

Success criteria: at least one copy-pasteable path to check or run Zith-- has
been verified from a clean command invocation.

## Step 3 - Prepare the website article

Goal: the About page introduces Zith-- and its history without conflating it
with Zith or overstating the compiler.

Sub-steps:

1. Read the saved Portuguese draft at
   `/home/diogo/Zith/docs/plans/zith-minusminus-introduction-draft.md`.
2. Prepare and author-review an English version for the existing English About
   feed. Keep the distinction between the two languages and the approximate
   2024 origin date.
3. On the approved publication day, run `date +%F` and use its output as the
   post date and filename prefix.
4. Create
   `/home/diogo/Zith-website/about/posts/<date>-introducing-zith-minusminus.html`
   using the metadata and HTML structure of the existing About posts.
5. Add the post's `file`, `title`, and `date` to the first entry of
   `/home/diogo/Zith-website/about/posts/index.json`.
6. Link the article to the Zith-- specification, current implementation
   status, GitHub repository, and only the try-it path verified in Step 2.
7. Do not add "Explicit where it matters, Easy where it doesn't" as a public
   tagline. Explain the idea in ordinary prose if it helps the reader.
8. Inspect the rendered article before deployment. The website repository
   README says its live branch publishes changes, so do not push without the
   author's explicit approval.

Commands:

    date +%F
    agent-run -t 900 -c 0 -- make -C /home/diogo/Zith-website build
    agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-routing.test.js
    agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-editor.test.js

Expected output:

- `date +%F` prints one ISO date, for example `2026-11-06`.
- The site build exits with code `0`.
- Both website tests exit with code `0`.
- The new article appears in the generated site and the About feed links to it.

Failure checks:

- If the site build or either test fails, do not deploy. Fix the site source and
  repeat all three validation commands.
- If the post metadata date differs from the filename or index entry, correct
  all three values to the date printed by `date +%F`.
- If the article claims a feature absent from current `docs/impl-status.md`,
  remove the claim before rebuilding.

Success criteria: the site build and both tests pass, and the rendered post
contains accurate Zith-- links and one verified try-it path.

## Step 4 - Make one public announcement

Goal: interested developers can discover the project and understand what
feedback is useful.

Sub-steps:

1. Publish the reviewed About article only after Steps 1 through 3 pass and the
   author approves deployment.
2. Share the article and repository in one relevant channel at a time. Use the
   project's own Discord for follow-up discussion.
3. Consider Hacker News Show HN only if a user can actually try the language
   with the route verified in Step 2. A blog post or landing page alone is not
   a Show HN project.
4. The saved draft is AI-assisted. Do not paste it, lightly edit it, or
   summarize it as Hacker News submission text. If using Show HN, write the
   title, submission text, and comments from scratch in your own words, link to
   the runnable project, and check the current Hacker News guidelines.
5. Explain why the project exists, how to try it, what is incomplete, and what
   technical feedback would help. Stay available to answer questions.
6. Do not ask friends to upvote, coordinate comments, or repost the same
   announcement repeatedly.

Expected output:

- One approved first-party article is live.
- Any Show HN submission links to a tryable Zith-- project and is authored by
  the project creator in their own words.
- The post states the beta status and links to the current implementation
  status.

Failure checks:

- If no tryable Zith-- path is available, do not submit a Show HN. Keep the
  first-party article as the announcement and wait until the try-it path is
  ready.
- If any channel's rules prohibit the post, do not post there.
- If the creator cannot be available to discuss the project, delay the
  community announcement.

Success criteria: one accurate announcement is published, with no unsupported
feature claims or coordinated voting.

## Step 5 - Collect and use feedback

Goal: the first two weeks of public feedback produce concrete follow-up work
instead of vanity metrics.

Sub-steps:

1. For 14 days after publication, review replies and issues at least twice a
   week.
2. Record recurring questions about the language distinction, setup, current
   compiler support, and design goals.
3. Answer factual questions with links to the relevant specification or
   implementation status.
4. Turn verified documentation gaps into documentation changes. Turn feature
   requests into proposals only after evaluating them separately from the
   publicity plan.
5. Do not claim success from views, stars, or votes alone.

Expected output:

- A short record of recurring questions and useful feedback exists after day
  14.
- Any public clarification matches the current compiler and language
  specifications.

Failure checks:

- If feedback reveals a mismatch between public copy and compiler behavior,
  correct the copy promptly and pause further promotion until the mismatch is
  resolved.
- If no useful feedback arrives, do not manufacture a success claim. Keep the
  article available and continue development at the author's pace.

Success criteria: after 14 days, the author has a concise list of actual
questions, confirmed documentation gaps, and any follow-up decisions.

## Final Acceptance

Promotion is ready only when the compiler build, complete CTest suite, example
check and run, website build, and website tests all pass. The article must
identify Zith-- as the compiler target, distinguish it from Zith, link the
current implementation status, and make no unverified feature claims. A Show
HN submission additionally requires a working try-it path and original,
human-written submission text.

## References

- [Show HN guidelines](https://news.ycombinator.com/showhn.html)
- [Hacker News guidelines](https://news.ycombinator.com/newsguidelines.html)
