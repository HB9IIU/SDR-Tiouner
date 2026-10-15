# Contributing to SDR-Tiouner

Thank you for helping. Bug reports, test results from your radio and pull requests are all welcome.

## Reporting a problem

Please report problems to this project, not to the OnAir team: SDR-Tiouner is a modification of OnAir and the problem is most likely in my part. Only if you can reproduce it with the original OnAir program, report it to OnAir as well.

Open an issue and include:

- the SDR-Tiouner version (shown in the page header and in the console at start),
- your radio model and how it is connected (USB 2/3, network),
- what the page showed: the status line, the card state and, if you can, a screenshot,
- the console output of `SDR-Tiouner.exe` if something failed to start.

A radio that does not list, or that lists but does not deliver samples, is the most common problem: please say which of the two it is.

## Test results

Reports of which radios work for you, with the gain and the symbol rates you received, are very valuable. Please say what you could not receive too.

## Pull requests

- Build and run the tests first (`ctest --test-dir build-windows`); add a test for new behaviour.
- Keep a change focused. Explain what it does and why in the description.
- The receiver and signal-processing core (`core/`) comes from [OnAir](https://github.com/adswill/OnAir). Improvements there are best sent to OnAir as well, so that both projects benefit.
- By contributing you agree that your work is licensed under the GPL, like the rest of the project.

## Building

See the "Build from source" section of the [README](README.md).
