# git-commit-ui

A TortoiseGit-style commit dialog for Omarchy, written in C++ / Qt 6.
It shows a checkable file list and message editor on the left and a side-by-side
diff of the selected file on the right, all colored by your current Omarchy theme.

## Build & install

```sh
sudo pacman -S --needed qt6-base cmake base-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build        # installs /usr/local/bin/git-commit-ui
```

## Use

```sh
git-commit-ui              # repo containing the current directory
git-commit-ui ~/code/app   # a specific repo
```

Launched from a menu with no repo, it asks you to pick one.

Make it a git subcommand (`git ui`):

```sh
git config --global alias.ui '!git-commit-ui'
```

Hyprland binding that opens it for the repo of the focused terminal
(add to `~/.config/hypr/bindings.conf`):

```
bind = SUPER SHIFT, G, exec, git-commit-ui "$(omarchy-cmd-terminal-cwd)"
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
- **It uses the real `git` binary**, so hooks, GPG/SSH signing and credential
  helpers work as they do in your terminal. Commit and push run in the
  background, so slow pre-commit hooks don't freeze the window.
- **Amend** pre-fills the last message. With no files checked, it only rewords.
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
- Stage/commit individual hunks from the diff pane
- File context menu: revert, add to .gitignore, open in editor
- Connector ribbon between the diff panes (TortoiseGitMerge-style)
- PKGBUILD for the AUR
