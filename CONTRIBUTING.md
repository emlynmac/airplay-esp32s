# Contributing

## Branches

**Open pull requests against `staging`, not `main`.**

`staging` is the integration branch. Every push to it replaces the rolling `beta`
pre-release, so merged work is immediately installable from the browser installer and can
be tried on hardware before it reaches anyone running a release. `main` carries stable
releases and moves only when a version is tagged.

## Formatting

This project uses `clang-format` 22.1.4 for C and header files. Use the pinned
development dependency so local formatting matches CI:

```sh
python3 -m pip install --user -r requirements-dev.txt
scripts/format.sh
```

To check formatting without changing files:

```sh
scripts/format.sh --check
```
