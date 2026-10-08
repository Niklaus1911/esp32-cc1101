# Finish privacy cleanup before publication

The sanitized main branch was committed, pushed, and verified through a fresh clone. GitHub continued to resolve earlier commits by their exact hashes in the original repository, so the user approved keeping that repository as a private backup and creating a fresh private repository for the sanitized history.

Steps approved and completed on 2026-10-09:

1. Rename the existing private repository to `esp32-cc1101-private-backup`. The original repository had no issues, pull requests, stars, forks, releases, or Actions runs to migrate.
2. Create a fresh private `esp32-cc1101` repository under the same owner, without an initial README or other generated commit.
3. Push only the verified sanitized main branch into the fresh repository. Preserve the original local checkout and its unpublished work.
4. Verify the fresh repository's head, tree, authorship, and complete reachable history. Check that the known original commit hashes cannot be resolved in the fresh repository.
5. Keep both repositories private. Making the fresh repository public requires a separate user instruction.

Verification: GitHub reports no commit found for all 72 original commit hashes in the fresh repository; the private backup still resolves the original history. The original local checkout points to the backup and retains its unpublished work. The sanitized checkout keeps its actual hardware port mappings only in ignored local configuration.

This changes the GitHub repository identity while retaining the original name for the clean repository. A new repository may need to be selected in integrations that grant access by repository ID.

GitHub documents retained cached commits after history rewrites in its [sensitive-data removal guidance](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository).
