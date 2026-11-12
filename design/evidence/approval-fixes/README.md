# Approval-fix verification

The full expanded matrix passed 45 rows with no failures. See matrix/matrix.tsv
and matrix-console.txt. Guest transcripts are in matrix/ (terminal trailing
spaces and carriage returns normalized for version control). The stock-mkfs
failure in the root-hole regression is an intentional before-fix reproduction.

stage-*.txt identifies the working tree used for the full run. Current kernel
and regression-program files match that staged source. Subsequent wrapper and
demo adjustments were verified with the transcripts in demos/ and a fresh
normal-build ask/approval follow-up (two repeats on 3 CPUs) in final/.

Agent tests: 20 pass. Tooling tests: 3 pass. No live LLM/API requests were used.
All approvals in automated runs are explicitly scripted by the host harness,
not synthesized or bypassed inside the kernel. Human-facing agent invocations
ask interactively unless the caller explicitly supplies --answer.

Reports enumerate changed blocks and show bounded byte previews, not a full
file diff. A report that exceeds its 4 KiB text buffer is refused. Normal
build images were restored after validation. Historical performance figures
were not rerun under this new approval policy.
