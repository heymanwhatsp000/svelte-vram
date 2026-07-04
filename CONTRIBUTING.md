# Contributing to Svelte

## Reporting Bugs

Use the NexusMods bug tracker for the mod page. Include:

1. Game name and version
2. GPU model and driver version
3. `svelte.ini` contents
4. `svelte.log` file (attach as a spoiler or pastebin link)
5. What you expected vs what happened

## Building from Source

See `README.md` for build instructions. You need Visual Studio 2022 Build Tools with the C++ workload.

## Pull Requests

Pull requests are welcome. Before submitting:

1. Build both D3D11 and D3D9 to verify your changes compile
2. Test in-game (at least one game per API)
3. Keep changes focused - one feature/fix per PR
4. Update CHANGELOG.md

## License

By submitting a pull request, you agree that your contributions are licensed under the MIT License. See `LICENSE` for details.

## Code Style

- C++14
- MSVC `cl.exe` compiler
- Tab indentation
- Comments explain WHY, not WHAT
- No version-history comments in source (use CHANGELOG.md)
