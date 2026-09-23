# og — Omarchy Git

A TortoiseGit-style Git UI for Omarchy, written in C++ / Qt 6.
The commit dialog (`og commit`) is the first subcommand.
It shows a checkable file list and message editor on the left and a side-by-side
diff of the selected file on the right, all colored by your current Omarchy theme.

## Build & install

```sh
sudo pacman -S --needed qt6-base cmake base-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
```

That installs `~/.local/bin/og` and
`~/.local/share/applications/omarchy-commit.desktop`, and needs no root.
Drop the `CMAKE_INSTALL_PREFIX` line to install under `/usr/local` for every
user on the machine instead (`cmake --install` then needs `sudo`).

**Install it rather than running it out of `build/`.** xdg-desktop-portal
resolves the app by looking up `omarchy-commit.desktop` and then the `og` on
its own `PATH`; until both exist it logs

```
qt.qpa.services: Failed to register with host portal ... App info not found
```

which is harmless but noisy. Both `~/.local/bin` and `~/.local/share` are in
the portal's search path, so a user install is enough to silence it.

## Use

```sh
og                  # commit dialog for the repo containing the current directory
og commit           # the same thing, spelled out
og commit ~/code/app  # a specific repo
```

Launched from a menu with no repo, it asks you to pick one.

| Subcommand | Status |
| --- | --- |
| `og commit` | Working — the dialog below. Also the default when none is given. |
| `og log` | Working — history browser, see below. |
| `og resolve` | Planned |

`og --help` lists these; `og --version` prints the version.

Hyprland binding that opens it for the repo of the focused terminal
(add to `~/.config/hypr/bindings.conf`):

```
bind = SUPER SHIFT, G, exec, og commit "$(omarchy-cmd-terminal-cwd)"
```

The window's class / app_id is `omarchy-commit`, so you can target it with a
float/size window rule (the rule syntax depends on your Hyprland version).

## Keyboard

| Key | Action |
| --- | --- |
| Ctrl+Enter | Commit |
| Ctrl+Shift+Enter | Commit & push |
| Alt+Down / Alt+Up | Next / previous change in the diff |
| Space | Toggle the selected file |
| Ctrl+F | Filter files |
| Ctrl+S | Save edits made in the diff |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo edits in the diff |
| F5 | Refresh |
| Esc | Close (the message is kept as a draft) |

## How it behaves

- **Checked files are exactly what gets committed.** It uses `git commit --only`,
  so anything else you had staged stays staged but is left out, as in TortoiseGit.
  Untracked files start unchecked; checking one adds it.
- **The diff is working tree vs HEAD**, i.e. what the commit will contain.
- **The right side of the diff is editable**, and nothing is written until you
  save. Type into it directly, or right-click to take from the left (HEAD) as in
  TortoiseGitMerge: *Use left text block*, *Use left line*, *Use text block from
  left before right*, or *Use left whole file*. The diff re-computes as you go.
  Unsaved edits mark the file `●` and show a **Save** button (Ctrl+S); Ctrl+Z /
  Ctrl+Shift+Z undo and redo. Switching files, committing or closing with unsaved
  edits asks whether to save them — a commit takes files from disk, so unsaved
  edits would otherwise be left out. Saving keeps the file's line endings, and if
  the file changed on disk since it was opened, asks before overwriting. Editing
  is offered for plain modifications of UTF-8 files; others stay read-only.
- **It uses the real `git` binary**, so hooks, GPG/SSH signing and credential
  helpers work as they do in your terminal. Commit and push run in the
  background, so slow pre-commit hooks don't freeze the window.
- **New branch**: fill in the branch box and the commit goes to a branch created
  from the current HEAD, leaving the branch you were on where it was. Leave it
  empty to commit to the current branch. A name git rejects, or one that already
  exists, is refused before anything is changed.
- **Amend** pre-fills the last message and adds the files the last commit
  already contains to the list, marked *In last commit* and checked. Uncheck one
  and it is taken out of the amended commit: its change returns to the working
  tree as a pending change rather than being lost. With no files checked, it only
  rewords. The new-branch box is disabled while amending.
- **Right-click a file → Revert…** takes it back to the last commit, dropping both
  staged and unstaged changes, after a confirmation. A newly added file (or the new
  name of a rename) is only un-added and stays on disk as untracked; untracked files
  have nothing to revert to and don't offer it.
- **Commit & Push** sets the upstream (`-u origin <branch>`) on a branch's first push.
- The window closes by itself when the working tree ends up clean.
- Recent messages (last 25) live under **Recent ▾**, and unsent drafts are
  saved per repo.

## Log

`og log` shows the history as a graph: each commit with its branch and tag
names (current branch bold, local branches, remotes and tags in their own
colours) and a ring on the commit HEAD is on. Selecting a commit shows its
message and details, and the files it changed under the graph; selecting a
file shows its diff against the commit's parent on the right. Merges are shown
against their first parent. History loads as you scroll, and *All branches*
switches between every branch, remote and tag and only the current branch.
F5 reloads, Esc closes.

## Theming

Colors come from the active theme's `colors.toml`
(`~/.config/omarchy/current/theme/`, with `~/.local/state/omarchy/current/theme/`
as a fallback). The app watches those paths and re-colors live when you switch
themes. A `light.mode` file in the theme switches to light-theme diff tints.
The font comes from `omarchy-font-current` (fallback: JetBrainsMono Nerd Font).

Colors used: `background`, `foreground`, `accent`, `selection_*`, `color1` (removed),
`color2` (added), `color3` (warnings).

## Roadmap ideas

- Syntax highlighting in the diff
- More file context menu entries: add to .gitignore, open in editor
- Connector ribbon between the diff panes (TortoiseGitMerge-style)
- `og resolve`
- PKGBUILD for the AUR
