# nxnative (Nextendo Edition)

Play your own Nintendo Switch and Switch 2 games natively on Apple silicon Macs, iPhone and iPad, with online multiplayer powered by the **Nextendo Network**. One script packs your game into a Mac app or an unsigned IPA that you sideload.

It is not an emulator: the game's own code runs directly on the Apple chip, with Switch graphics rebuilt on Metal. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.

> **About this fork**: `nxnative-nextendo` extends upstream nxnative with native online play and network service integration for [Nextendo Network](https://nextendo.network). It routes game network traffic through Nextendo servers via Darwin BSD sockets and TLS interception, integrates seamless in-app OAuth login with Apple Keychain token persistence, implements native modal UI for system Error Applets, and answers Horizon account and network service requests dynamically without requiring external proxy scripts.

[Games](#games) · [Community front ends](#community-front-ends) · [Requirements](#requirements) · [How to play](#how-to-play) · [Nextendo Network](#nextendo-network) · [Discussions](#discussions) · [For developers](#for-developers)

## Games

The full [tested games table](dm/games/README.md#games) includes original Switch games, tested builds, engines and graphics APIs. Each game's note records its tested platforms, progress and known issues.

These Switch 2 games have passed Mac gameplay testing. Build them from a [`.nup` file or a dump folder](#switch-2-games):

- [Donkey Kong Bananza](dm/games/040074A01BF12000/README.md)
- [Fast Fusion](dm/games/040041E0212AE000/README.md)
- [Fire Emblem Fortune's Weave](dm/games/04007EF01F8AE000/README.md) — up to 120 fps
- [Hyrule Warriors: Age of Imprisonment](dm/games/04003F40232DE000/README.md)
- [Kirby Air Riders](dm/games/040077201BE50000/README.md)
- [Mario Kart World](dm/games/0400C3F00006E000/README.md)
- [Mario Tennis Fever](dm/games/0400D4201C6EA000/README.md)
- [Nintendo Switch 2 Welcome Tour](dm/games/040057C02159A000/README.md) — Area A tested; exhibits need further testing
- [Nintendo Switch Sports Resort](dm/games/04008E001F098000/README.md)
- [Orbitals](dm/games/040032B023CA4000/README.md) — two players: a controller for player 2
- [Pokémon Pokopia](dm/games/0400A3A01EE4C000/README.md)
- [PRAGMATA](dm/games/0400C76024710000/README.md)
- [Splatoon Raiders](dm/games/0400380020056000/README.md)
- [Star Fox](dm/games/0400E88025B1A000/README.md)
- [Super Mario Bros. Wonder – Nintendo Switch 2 Edition + Meetup in Bellabel Park](dm/games/040015100B514001/README.md)
- [The Legend of Zelda: Ocarina of Time](dm/games/0400486024DEC000/README.md)
- [Yoshi and the Mysterious Book](dm/games/0400245021D2C000/README.md)

## Community front ends

Community projects build on nxnative with tools to turn your games into apps for your iPhone, iPad or Mac without the command line, and libraries to organize and launch them. They are separate projects: questions and bugs about them go to their own pages.

- [NX2Apple](https://github.com/LNPANDY/NX2Apple) — Mac builder with a shared iPhone/iPad game library.
- [NX Play](https://github.com/varyform/nx-native-wrapper) — Mac library with device installs and save sync.
- [NxNative Launcher](https://github.com/rennan101us/nxnative_Launcher) — Mac launcher with guided builds and controller mapping.
- [nxnative Easy](https://github.com/nalancer08/nxnative-easy) — Mac game library with guided builds, iPhone and iPad installation and signature renewal, and iCloud save sync.
- [LumaNX](https://github.com/Xc1Ym/LumaNX) — Mac game builder with IPA export.
- [GameCase](https://github.com/Tiaoge-tou/GameCase) — iOS game library based on LiveContainer.

## Requirements

- **Mac:** Apple silicon, macOS 26.6 or later. Building games requires Xcode 27.2.
- **iPhone:** A17 Pro chip or later, with iOS 26 or later.
- **iPad:** M1 chip or later, with iPadOS 26 or later.

The games' textures are BC-compressed, which older iPhones cannot read. Switch 2 builds are tested on the Mac; check each game's note for device-specific validation.

For a game without a profile, the script writes a survey and tries a provisional build with guessed settings. That build may not start; see [Port another game](docs/development.md#port-another-game).

## How to play

For a Switch 2 game, follow the [Switch 2 build instructions](#switch-2-games) with a `.nup` file or a dump folder.

### Build from the command line

You need a Mac with macOS 26.6 or later, [Xcode](https://developer.apple.com/download/all/?q=Xcode%2027.2) 27.2, [Homebrew](https://brew.sh) and an arm64 build of Python 3.12 or newer. To build an IPA, install Xcode's iOS platform (Xcode → Settings → Components).

The shaders are built with Xcode 27.2's Metal compiler. Select it before step 1: `sudo xcode-select -s /Applications/Xcode.app`. With several Xcode versions installed, name this one `Xcode-27.2.app` and select it with `sudo xcode-select -s /Applications/Xcode-27.2.app`.

**1. Install the tools** (only once), in Terminal:

```
xcodebuild -downloadComponent MetalToolchain
git clone https://github.com/seedds/nxnative.git && cd nxnative
brew install xcodegen zstd glslang
python3 -m pip install lz4 "capstone>=5,<6" fonttools nsz zbic "backports.zstd; python_version < '3.14'"
```

**2. Put your Switch 1 game in a folder.** Include any updates and DLC you want to pack. The script reads `.nsp`, `.xci`, `.nsz` and `.xcz` files.

```
~/Games/MarioWonder/
  Super Mario Bros. Wonder [010015100B514000][v0].nsp
  Super Mario Bros. Wonder [010015100B514800][v327680].nsp
```

**3. Build**, with your game folder and your `prod.keys`:

```
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys
```

Optionally supply Switch firmware with `--firmware ~/keys/Firmware.zip` (tested with firmware 22.5.0) to extract authentic system data, 3D Mii models, and shared system fonts into the bundle.

For a Mac app, add `--platform mac-app` and open the resulting `.app` in Finder. By default, the script saves the IPA or Mac app in the game's folder. See [The packing script in detail](docs/development.md#the-packing-script-in-detail) for all options.

The first build takes 30–60 minutes and needs about twice the game's size plus 10 GB of free disk space. Rebuilding is much faster.

### Install

Install the IPA on your iPhone or iPad with a sideloading tool such as AltStore, Sideloadly or LiveContainer. If your tool offers **Increased Memory Limit**, turn it on: the bigger games need it.

Game too big to install? Build again with `--romfs separate`, and run the command the script prints once to copy the game files to your phone ([Quick reinstalls](docs/development.md#quick-reinstalls)). With AltServer, also add `--shader-files`: it cannot install a file of 2 GiB or more.

If a sideloading tool fails to extract a large IPA, try `--prepare-xcode` and [install from Xcode](docs/development.md#build-and-run-on-a-device-with-xcode).

Controls, saves and settings: [Playing](docs/playing.md).

### Switch 2 games

Build a Switch 2 game from one [NUP](https://github.com/a55uka/nup) container (a `.nup` file) or a dump folder. Both inputs work without `prod.keys`. If the game has no profile in this project, the script surveys it and creates a provisional profile. The resulting build may fail to boot. See [Port another game](docs/development.md#port-another-game).

**From a `.nup` file**, which holds the base game, its updates and DLC in one file. Give the file, or a folder that holds it:

```
python3 scripts/build_ipa_from_nsp.py ~/Games/DonkeyKongBananza.nup --platform mac-app
```

[Hyrule Warriors: Age of Imprisonment](dm/games/04003F40232DE000/README.md) is played this way: its 47 GB NUP unpacks in under five minutes. A NUP that carries the game's control data (its NACP) and icon gives the app the game's name and icon; otherwise the name comes from the game's profile and the app has no icon unless you give it one with `--icon <image>` (`ICON` in Spyder; a JPEG or PNG of any size). The script also reads [NUPs with shifted metadata numbers](docs/reference/packing.md#a-nup-icon-is-written-as-control-data).

A Nintendo Switch 2 Edition of a Switch 1 game can hold both of its programs in one NUP: the Switch 1 program with the game data, and the Switch 2 program, which reads that data from the Switch 1 program and its Switch 2 files from the upgrade add-on. The script packs the Switch 2 program, with the Switch 1 program's game files beside its own and the add-on as DLC; `--program 0` packs the Switch 1 program instead.

A NUP without the base game can have an empty base record or omit the base record entirely. With no base record, every update must be marked `standalone_update`. In either case, the script reports that it is using the selected update as the whole game. That update needs a complete ExeFS and RomFS. If files are missing, pack a NUP with the base game. The script selects the newest update by default. Use `--package v<N>` to select another update. `v0` requires a base game.

**From a folder of files**. The script expects this layout (`updates` and `dlc` are optional):

```
~/Games/DonkeyKongBananza/
  base/exefs/                     main, main.npdm, sdk, rtld
  base/romfs/
  updates/458752/exefs/           the update's version, as a decimal number
  updates/458752/romfs/           the patched RomFS, not a patch
  dlc/040074a01bf13001/0/romfs/   the DLC's title ID
```

```
python3 scripts/build_ipa_from_nsp.py ~/Games/DonkeyKongBananza --platform mac-app
```

You can also pass a complete update folder directly, for example `~/Games/DonkeyKongBananza/updates/458752`. The script treats a folder with `exefs/` and `romfs/` at its root as `base/`, including folders made by `nup extract`. Use `--icon <image>` (`ICON` in Spyder) to supply an icon for the app. To turn a dump folder into a NUP, run `nup pack ~/Games/DonkeyKongBananza game.nup`. See [The packing script in detail](docs/development.md#the-packing-script-in-detail) for more options.

## Nextendo Network

This fork adds full, native integration with the [Nextendo Network](https://nextendo.network), an alternative game server replacement for Nintendo Switch games.

### Purpose and Features of this Fork

- **Native Online Play**: Enables multiplayer matchmaking, rankings, lobbies, and online services on Apple silicon Macs and iOS devices without requiring emulators or external proxy setups.
- **In-App Authentication**: Log in once with your Nextendo account directly from the game launcher on macOS or iOS. nxnative starts a local loopback listener to handle the OAuth 2.0 authorization callback and securely stores your token in Apple Keychain.
- **Seamless Service Redirection**: Horizon OS networking calls (`bsd`, `nifm`, `ssl`, `account`) are redirected in the native host runtime to Nextendo's infrastructure:
  - Game DNS queries to Nintendo servers (`*.srv.nintendo.net`, `accounts.nintendo.com`, etc.) resolve to Nextendo backend endpoints.
  - Native BSD socket implementation bridges guest TCP/UDP traffic directly to macOS/iOS network interfaces.
  - TLS sessions automatically negotiate ALPN (`h2`/`http/1.1`) and match target servers.
  - BAAS authentication tokens (RS256 JWTs) are dynamically generated locally using Apple's Security framework to authenticate sessions with Nextendo services.
- **Native Error Applet UI**: When games encounter network or connectivity issues, error codes (e.g. `2XXX-YYYY`) are presented using native modal dialogs (`UIAlertController` on iOS and `NSAlert` on macOS), ensuring players receive clear visual feedback.
- **Authentic Built-in Miis & Native Mii Editor**: Fully implements the 6 canonical Nintendo Switch guest presets (Guest A through Guest F) for games with Mii support (such as Mario Kart 8 Deluxe). When a game invokes the system Mii applet (`MiiEdit` / `nn::mii`), nxnative launches a native modal editor (`NSAlert` on macOS, `UIAlertController` on iOS) to customize nicknames, gender, colors, and presets synchronously.
- **Firmware Integration**: Supports packaging official Switch `Firmware.zip` archives directly via `--firmware` (tested with firmware 22.5.0). Extracts system data (`MiiModel` 3D meshes and textures, `MiiEdit` applet) and decodes shared system fonts (`standard.ttf`, `nintendo_ext.ttf`) into the application bundle for 100% authentic rendering.

See [Nextendo Network in Development](docs/development.md#nextendo-network) for architectural and implementation details.

### Tested Games

- **[Mario Kart 8 Deluxe](dm/games/0100152000022000/README.md)** (`0100152000022000`): Nextendo login, matchmaking, and online play verified on macOS.

## Discussions

Use [Discussions](https://github.com/seedds/nxnative/discussions) for questions, ideas and reports on how games run on your Mac or iPhone. File bug reports in [Issues](https://github.com/seedds/nxnative/issues) and include the log. Follow frame rate, resolution and which Macs hold 60 fps in [Ocarina of Time: state on main](https://github.com/seedds/nxnative/discussions/1789).

Share your game's `pso_user.tsv` in the pinned [prewarm lists discussion](https://github.com/seedds/nxnative/discussions/3291).

## For developers

[How it works](docs/development.md) · [Game notes](dm/games/README.md#games) · [Bring-up and debugging](docs/agents/README.md) · [Front ends](docs/frontends.md) · [Contributing](CONTRIBUTING.md)

## Acknowledgements

This project is built on other people's work ([licenses](dm/third_party/README.md)):

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected. nxnative started as a copy of its generic parts: the NSO rewriter, the host that stands in for Horizon, the NVN-on-Metal layer and the iOS app shell. Fixes go both ways.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2, in `dm/third_party/ryujinx` with a few fixes listed in [its README](dm/third_party/ryujinx/README.md)) turns the games' shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reads NSPs and layers updates over the base game in `dm/tools/nx_extract`.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language.
- **[glslang](https://github.com/KhronosGroup/glslang)** compiles the GLSL of OpenGL games to SPIR-V for it.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT): the GPU widening of 8-bit indices and in-pass clears in `dm/gfx/nvn/nvn_mtl.m`, and the visibility-counter lowering in the shader translator.

## License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license ([dm/third_party](dm/third_party/README.md)).
