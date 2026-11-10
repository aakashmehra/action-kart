# Action Kart

A native iPhone and iPad build of a 2005 DS kart racer, without an emulator. The game's code from your own cartridge
dump is translated to C on your Mac and compiled into an app for your own device. Adds a one-screen 16:9 layout, a
wider 3D camera, a redesigned pixel-art interface and touch controls.

Based on [DS Kart Recompiled](https://github.com/AlonsoTlol/DS-Kart-Recompiled) by alontube (MIT).

## Legal

- **Unofficial fan project.** Not affiliated with, endorsed or sponsored by Nintendo. Mario Kart DS and all related
  names, characters and trademarks belong to Nintendo.
- **No game included.** This repository contains no ROM, no game code and no game assets. You need your own legally
  dumped copy of the European version (game code AMCP).
- **Everything game-related is generated on your machine** from your copy and is ignored by git (`rom/`,
  `generado/`, `extraido/`, `build/`). Never commit or share those files.
- **No public builds.** There is no IPA, app store listing, TestFlight or release download, and there never will be.
  The app you build contains code translated from your copy of the game: it is for your own devices only. Do not
  share it.

See [RIGHTS_AND_LICENSES.md](RIGHTS_AND_LICENSES.md) for every component's license.

## Build your own app

You need a Mac with Xcode (with the iOS SDK), [Homebrew](https://brew.sh) with `brew install sdl2 cmake`, and your own
ROM.

1. Copy your ROM to `rom/mkds.nds`.
2. Run `sh ios/construir.sh`. The first run translates your ROM's code and compiles everything (it can take a while;
   `NUCLEOS=2` uses fewer cores). It writes `build/ActionKart.ipa`, unsigned.
3. Install it on your own iPhone or iPad with Sideloadly, AltStore or SideStore, signed with your own Apple ID.
4. On first launch, tap **Choose ROM** and pick the same ROM. The app only accepts the ROM it was built from.

`sh ios/construir.sh simulador` builds and opens it in the iOS Simulator instead.

## Community

Join the Discord for build help, bug reports and online races: **https://discord.gg/Cn6AEY8VCR**

Server rules:

1. Action Kart is an unofficial fan project, not affiliated with or endorsed by Nintendo.
2. **No ROMs, no IPAs, no game files and no download links**, in chat or in DMs. Everyone builds their own app from
   their own legally dumped copy. Breaking this means an instant ban.
3. No piracy talk: don't ask or tell where to get the game.
4. Build problems go in #build-help (include the error, your Mac model and macOS version); bugs go in #bug-reports
   with a screenshot.
5. Be kind. No harassment, hate, spam, NSFW or self-promotion.
6. Moderators have the final say.

## Credits

- alontube, for DS Kart Recompiled (recompiler, runtime, online rooms).
- Dear ImGui (MIT), Jersey 10 and Lilita One fonts (SIL Open Font License 1.1), SDL2 (zlib, downloaded at build time).
