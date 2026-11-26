# Rekodr: rules for anyone working on it

Rekodr is not released. One PC runs it. Write for the program as it is going to be, not for what it was.

## No backwards compatibility
- Never keep an old format, setting, file layout or API field working "for older versions". There are none.
- When a format changes, change it and delete the old path in the same commit. No fallbacks, no migrations, no
  "read the old key too", no version numbers in keys kept for old data.
- This holds until the first public release. After that, ask before breaking something players already have.

## Comments and READMEs say what the code does now
- No history ("used to", "no longer", "the Rust version did", "X agreed on <date>") and no plans or stages.
  History goes in commit messages. A plain `TODO:` is fine.
- READMEs are short: what the program is and how it works. No action plans, no lists that restate settings.
- Rekodr and its service link to each other: https://github.com/CalvBot/rekodr and
  https://github.com/CalvBot/rekodr-api.

## Builds
- Builds run locally (`app\build.bat`). A release happens only when the maintainer asks. While the repo is private its
  releases are built locally too; once public, a release is built on GitHub Actions, once, never on every push.

## The service (rekodr-api)
- Its address tells nothing to someone without the app: every unsigned request, the front page too, is an empty 404.
