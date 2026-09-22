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
| `og log` | Planned |
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
| F5 | Refresh |
| Esc | Close (the message is kept as a draft) |

## How it behaves

- **Checked files are exactly what gets committed.** It uses `git commit --only`,
  so anything else you had staged stays staged but is left out, as in TortoiseGit.
  Untracked files start unchecked; checking one adds it.
- **The diff is working tree vs HEAD**, i.e. what the commit will contain.
- **Right-click the diff to take from the left** (HEAD) into the file on disk, as in
  TortoiseGitMerge: *Use left text block*, *Use left line*, *Use text block from
  left before right*, or *Use left whole file*. The file is rewritten immediately,
  keeping its line endings; *Undo last change* in the same menu puts it back, even
  if the edit left the file clean and it dropped out of the list. Available for
  plain modifications of UTF-8 files; if the file changed on disk since the diff
  was shown, the edit is refused and the diff reloaded.
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
- **Commit & Push** sets the upstream (`-u origin <branch>`) on a branch's first push.
- The window closes by itself when the working tree ends up clean.
- Recent messages (last 25) live under **Recent ▾**, and unsent drafts are
  saved per repo.

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
- File context menu: revert, add to .gitignore, open in editor
- Connector ribbon between the diff panes (TortoiseGitMerge-style)
- `og log` and `og resolve`
- PKGBUILD for the AUR
