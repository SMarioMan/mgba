# Project commit identity

After cloning, run these commands from the repository root:

```sh
git config --local user.name SMarioMan
git config --local user.email 13172667+SMarioMan@users.noreply.github.com
git config --local user.useConfigOnly true
git config --local core.hooksPath .githooks
```

The tracked `commit-msg` hook keeps this co-author in a separate trailer block:

```text
Title

Optional description.

Co-Authored-By: GPT-6.1 Sol (Medium) <codex@users.noreply.github.com>
```

It also repairs an existing misplaced or repeated Codex co-author. Git for
Windows runs this POSIX shell hook through its bundled shell. The hook leaves
empty or comment-only messages empty so cancelling an editor still cancels a
commit. Repository-local identity settings take precedence over global settings;
Git does not activate tracked hooks automatically in a fresh clone.

The October 17 history repair preserved every project commit's tree, original
parent outside the project branch, author, committer and timestamps. Only project
commit messages and their descendant parent IDs changed. Local backup branches
`backup/pre-history-sync-20261017` and
`backup/upstream-before-trailer-repair-20261017` retain the previous histories.
Remote history has not been updated by this repair. Coordinate any subsequent
history publication and use `--force-with-lease`, never an unconditional force
push, so newer remote work cannot be overwritten silently.
