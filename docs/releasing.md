# Releasing

Normal main pushes run regression CI only. Publish only on an explicit release
instruction; pushing code alone is not authorization to create a version tag.

1. Choose an unused version (first beta: v0.1.0-beta.1), review the exact commit,
   and add docs/releases/<version>.md with changes and limitations.
2. Run the Release regression target locally, commit, push main, and verify CI.
3. Create an annotated version tag on that exact commit and push only that tag.
4. The Release workflow rebuilds and tests the tagged source, packages only
   public files, and creates a draft with a versioned x64 ZIP and SHA-256 checksum.
5. Verify the successful workflow, draft target, downloaded ZIP checksum,
   internal manifest, and excluded files before publishing the draft.
6. Mark versions with a suffix as prereleases. Report the published URL.

Do not replace a published asset or move an existing version tag. Fixes get a
new version. Failed validation must not be published. If asked for a draft only,
stop before publication. Real-GPU NR validation remains separate from hosted CI.

GitHub release creation requires contents:write; build/test workflows otherwise
use contents:read. No private runtime or game video is shipped in the app ZIP.
