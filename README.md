# og — Omarchy Git

A TortoiseGit-style Git UI for Omarchy, written in C++ / Qt 6.
The commit dialog (`og commit`) is the first subcommand.
It shows a checkable file list and message editor on the left and a side-by-side
diff of the selected file on the right, all colored by your current Omarchy theme.

## Build & install

### Omarchy / Linux

```sh
sudo pacman -S --needed qt6-base qt6-svg cmake base-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
```

That installs `~/.local/bin/og`,
`~/.local/share/applications/omarchy-commit.desktop` and its icon
(`~/.local/share/icons/hicolor/scalable/apps/omarchy-commit.svg`), and needs no
root.

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

### Windows: single executable

The released `og.exe` needs no MSYS2 or Qt installation on the machine where
it runs. MSYS2 is **only the build toolchain** used to make that executable.
To build it yourself, install [MSYS2](https://www.msys2.org/), open its
**UCRT64** shell, and run:

```sh
pacman -Syu
# If MSYS2 asks you to close the shell, reopen UCRT64 and run pacman -Syu again.
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-static \
  mingw-w64-ucrt-x86_64-libwebp
cd /d/path/to/omacommit
cmake -S . -B build-standalone -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/ucrt64/qt6-static -DOG_STANDALONE=ON
cmake --build build-standalone -j 4
./build-standalone/og.exe --version
```

Copy `build-standalone/og.exe` anywhere and launch it directly (including by
double-click). It links Qt, its plugins, codecs and the compiler runtime into
the executable; Windows system DLLs are still used. **Git for Windows must be
installed and on PATH** for repository operations: og invokes the real `git`
binary so Git hooks, signing and credential helpers work. The Omarchy theme
and agent are optional on Windows; without them og uses fallback colours and
disables **Write**.

Static linking Qt under LGPL has additional license obligations when
redistributing the executable (including providing a way to relink it with a
modified Qt); check your Qt licensing terms before distribution.

## Use

```sh
og                  # commit dialog for the repo containing the current directory
og commit           # the same thing, spelled out
og commit ~/code/app  # a specific repo
```

Launched from a menu with no repo, it asks you to pick one in the desktop's file
chooser (the XDG portal: Strata on Omarchy), or Qt's own dialog without a portal.

| Subcommand | Alias | What it opens |
| --- | --- | --- |
| `og commit` | `og c` | The commit dialog below. Also the default when none is given. |
| `og log` | `og l` | The history browser, see below. |
| `og resolve` | `og r` | Conflict resolution, see below. |

A directory literally named `c`, `l` or `r` needs a path prefix (`og ./l`).

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
- **Side by side or inline.** The ◫ / ☰ button next to ↑↓ switches the diff
  between two columns and one (removed lines above added, with old and new line
  numbers), in every window; the choice is remembered. When there isn't room for
  two readable sides it shows inline anyway, with the icon in the accent colour —
  click it for side by side regardless. Inline is read-only: typing needs the two
  sides, but the right-click *Use left …* actions still work.
- **Images are shown as pictures** — PNG, JPEG, GIF, WebP and whatever else Qt
  can read — before and after side by side (one above the other in inline mode),
  over a checkerboard so transparency shows, fitted but never enlarged, with
  each version's size in pixels and bytes. A new or deleted image shows the one
  side it has. An SVG opens as its text diff, with a *Picture* / *Source*
  button to switch (remembered). In `og resolve`, a conflicted image shows both
  sides above the buttons that pick one.
- **Whitespace** (the ⋯ button, remembered, in every window): *Show whitespace*
  draws spaces as `·` and tabs as `→`; *Ignore whitespace changes* hides changes
  that only add, remove or re-indent whitespace (`git diff -w`), with a
  *whitespace ignored* note in the header. While ignoring, the diff is read-only —
  git shows the unchanged-looking lines with the new side's whitespace, so
  *Use left* couldn't restore the old — and the option waits until unsaved edits
  are saved or discarded. It only changes what you see, never what is committed.
- **Whitespace problems are flagged** in `og commit`, on the lines you are adding:
  trailing whitespace, a space before a tab in the indent, a blank line added at
  the end of the file — whatever `git diff --check` would report, under your
  `core.whitespace` and `.gitattributes`. The spot is marked in red (hover it for
  what's wrong) and the header counts them; it never blocks a commit. Existing
  whitespace in lines you didn't touch isn't flagged, and neither is a CRLF line
  ending. Markdown uses two trailing spaces for a line break, which git flags
  too; to allow them for both git and og, add to `.gitattributes`:

  ```
  *.md whitespace=-blank-at-eol
  ```
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
- **✨ Write** uses Claude Code, Codex or OpenCode when installed on `PATH`.
  On Omarchy, its configured default agent is preferred; the button's menu lets
  you choose among installed agents. It sends the diff of the
  *checked* files (against the parent when amending), the last dozen commit
  subjects so it matches the repository's style, and your draft if you started
  one; the reply replaces the message as one undo step, so Ctrl+Z restores what
  you had. Nothing is sent until you click. Claude runs with no tools, Codex
  in a read-only sandbox, and OpenCode with permissions denied and default
  plugins disabled. **■ Stop** cancels it. The button is hidden when none are
  installed.
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
against their first parent. Right-click a commit → **Revert changes by this commit…** undoes its changes in
your working tree without committing (TortoiseGit's revert), so you can review
them; then **Open commit dialog** starts from git's prepared *Revert "…"*
message, or **Resolve…** if undoing it conflicts with later changes. A merge is
undone relative to its first parent. History loads as you scroll, and *All branches*
switches between every branch, remote and tag and only the current branch.
F5 reloads, Esc closes.

## Resolve

`og resolve [path]` resolves merge conflicts, TortoiseGitMerge style: the
conflicted files on the left, the two sides on top (aligned, read-only) and
the merged file below (editable, what gets saved). Pass a conflicted file to
open it first; `og commit` also offers **Resolve…** on a conflicted file.

- Your own work is always on the right, as in every other diff in og, and
  the sides are named by what they are, never "ours"/"theirs" — which git
  swaps during a rebase. Merging: *Theirs — feature/x* | *Mine — HEAD (main)*.
  Rebasing: *Upstream — main + 1 of your commits, already replayed* | *Mine —
  replaying abc123 …*. Buttons use the same names in the same left-to-right
  order, and yours is one colour, the other side another, in all three panes.
- A conflict is git's `<<<<<<< / ======= / >>>>>>>` block in the file. For
  each one: *Use theirs*, *Use mine*, or both in either order; or edit the
  merged text by hand — delete the markers and it counts as resolved. Alt+↓ /
  Alt+↑ move between conflicts and the top panes follow. Ctrl+Z undoes.
- *Whole file ▾* takes one side for the entire file.
- **Save** (Ctrl+S) writes the file; **Mark resolved** saves and stages it,
  asking first if conflicts remain.
- Binary files, and files deleted on one side, get whole-file choices: use
  or keep one side, or delete it.
- Once nothing is left unresolved, **Commit merge…** opens `og commit` with
  git's prepared message (also for cherry-picks and reverts). In a rebase,
  **Continue rebase** replays the next commit (`git rebase --continue`, keeping
  its message): the window moves on to that commit's conflicts, with the sides
  named for it, or says the rebase is finished.
- The file list counts each file's conflicts as you resolve them (`● ` marks
  unsaved changes), not only when it is reloaded.

## Theming

Colours come from the active Omarchy theme's `colors.toml`, in
`~/.local/state/omarchy/current/theme/` (or `~/.config/omarchy/current/theme/` on
older Omarchy releases). og watches it and re-colours open windows live when you
switch themes. The font comes from `omarchy-font-current` (fallback: JetBrainsMono
Nerd Font).

| Key | Used for |
| --- | --- |
| `background`, `foreground` | Everything; secondary text, borders and input fields are blended from these |
| `accent` | Focus, primary buttons, your side when resolving, the current branch |
| `selection` | Selected rows and text |
| `red` | Removed lines, conflict markers, errors |
| `green` | Added lines, added files |
| `yellow` | Modified files |
| `mode` | `light` switches to light-theme diff tints |

Colours that must be told apart — the log graph's lanes, the ref badges, the
two sides when resolving, and added vs removed lines — are picked from the whole
palette (`green`, `yellow`, `blue`, `magenta`, `cyan`, `orange`, `bright_*`, …)
so each is clearly distinct from its neighbours and readable on the background.
Many themes set `accent` equal to `blue`, and some (Osaka Jade, White,
Vantablack) are nearly one hue; greyscale themes fall back to lighter and darker
shades.

The terminal-style names older themes used (`color1`–`color4`,
`selection_background`) are still accepted.

## Roadmap ideas

- Syntax highlighting in the diff
- More file context menu entries: add to .gitignore, open in editor
- Connector ribbon between the diff panes (TortoiseGitMerge-style)
- PKGBUILD for the AUR
