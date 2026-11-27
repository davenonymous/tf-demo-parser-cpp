# Security

libtfdemo reads demo files, and demos often come from strangers: downloaded from demo sites, sent in by
players, uploaded to a service. A crash or a memory error on a crafted demo is therefore a security problem,
not just a bug.

To find such problems early, CI fuzzes on every change under AddressSanitizer and UndefinedBehaviorSanitizer:
the parser with the match and direct hit analysers and the game state, re-encoding, the anonymizer, the pose
splice, and the net message writers ([docs/development.md](docs/development.md#fuzzing)). The C API is not
fuzzed itself; its functions wrap the fuzzed C++ code.

## Supported versions

Only the latest release is supported. Fixes are released as a new patch version of it.

## Reporting a vulnerability

Please report it privately, through GitHub's private vulnerability reporting: open the repository's
**Security** tab and choose **Report a vulnerability**. Do not open a public issue.

Include:

- the demo or input that shows the problem, or how to make one;
- the version or commit you tested;
- what happens, such as the sanitizer report or the crash.

You get an answer within a week. The fix is released before the details are published.
