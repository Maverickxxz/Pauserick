# Releasing Pauserick

Pauserick updates itself from the GitHub releases of this repository. It installs a
release only if `Pauserick.exe` comes with a `Pauserick.exe.sig` made with the
maintainer's private signing key.

The exe is **built by GitHub Actions**, from the code in the repository, so anyone can
check where it comes from. The **signature is made on the maintainer's PC**: the private
key is never uploaded to GitHub, so even someone who took over the GitHub account could
not publish an update that Pauserick would install.

## Making a release (all on github.com, except one step)

1. **Raise the version** in `src/version.h` (`PAUSERICK_VER_MAJOR/MINOR/PATCH` and
   `PAUSERICK_VER_STR`, for example 1.0.0 -> 1.0.1) and commit. It can be done in the
   GitHub web editor.
2. **Build:** on GitHub, **Actions > release > Run workflow**. After 2-3 minutes,
   **Releases** shows a draft "Pauserick 1.0.1" with `Pauserick.exe` attached. The draft
   notes contain the exe's SHA-256. Drafts are invisible to the updater.
3. **Sign (on your PC):** download `Pauserick.exe` from the draft and drag it onto
   `sign-release.bat` in the repository folder. It writes `Pauserick.exe.sig` next to the
   exe and prints the exe's SHA-256: it must match the one in the draft notes. No compiler
   is needed, only the key (see below).
4. **Publish:** edit the draft, replace the notes' first part with what's new (the update
   dialog shows the notes under "What's new"), attach `Pauserick.exe.sig`, and publish.
   Do not mark it as a pre-release: the updater only sees the latest full release.

Installed copies find the new release within a day, or at once with **Check for updates**
in Settings. A release without `Pauserick.exe.sig` is ignored, never installed.

Without GitHub: `build.bat` builds `dist\Pauserick.exe` locally; drag that onto
`sign-release.bat` and attach both files to a release by hand.

## The signing key

- It lives in `%LOCALAPPDATA%\Pauserick-signing\pauserick-update-private.key`, outside the
  repository. Never commit it, never upload it, never put it in a GitHub secret.
- **Keep a backup** off this PC too (a password manager, an encrypted USB stick). If it is
  lost, no update can ever reach existing installs again: users would have to download a
  new version by hand, built with a new key.
- If it leaks, anyone could sign an update. Then make a new key
  (`build\pksign.exe keygen <new.key> src\update_key.h`), publish a version built with it,
  and tell users to download that version by hand once.
- The public half is in `src/update_key.h` and is compiled into every exe.

## What the updater checks

- The release tag must be a plain version (`v1.0.1`) newer than the running one.
- The `.sig` file must name that same version, and its ECDSA P-256 signature must match
  `SHA-256("PAUSERICK-UPDATE\n" + version + "\n" + exe)` under the built-in public key.
  Binding the version means an old signed exe cannot be passed off as a newer one.
- Only then is the exe written, next to the running one, and swapped in.
