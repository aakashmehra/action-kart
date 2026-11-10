# Rights and licenses

This repository is source code only. Each part keeps its own license and copyright; nothing here relicenses anyone
else's work.

## What is in this repository

| Part | Where | License |
|---|---|---|
| DS Kart Recompiled (the recompiler and runtime this project is based on), by alontube | `tools/`, `runtime/`, `linux/preparar.sh`, `faltan*.txt` | MIT, see `LICENSE` and `THIRD_PARTY_NOTICES/DS-Kart-Recompiled-MIT.txt` |
| Action Kart changes (one-screen 16:9 layout, wide 3D camera, interface, touch controls, ROM picker, iOS builder) | same folders and `ios/` | MIT, same terms as the project it is based on |
| Dear ImGui and its SDL_Renderer backend | `runtime/imgui/` | MIT, see `THIRD_PARTY_NOTICES/Dear-ImGui-MIT.txt` |
| Jersey 10 font (embedded in `runtime/letra_pixel.inc`) | The Soft Type Project Authors | SIL Open Font License 1.1, see `THIRD_PARTY_NOTICES/Jersey10-OFL.txt` |
| Lilita One font (used to draw the Skyendo logo image in `runtime/skyendo.c`) | Juan Montoreano | SIL Open Font License 1.1, see `THIRD_PARTY_NOTICES/LilitaOne-OFL.txt` |
| SDL2 | downloaded by `ios/construir.sh` at build time, not stored here | zlib license |
| "Action Kart" and "Skyendo" names, the app icon, the Skyendo logo and the interface design | original to this project | MIT, same as the code |

## What is not in this repository, and must never be committed or shared

- The game (any `.nds` ROM), in any region.
- Anything generated from a ROM: the translated game code (`generado/`), extracted code and data (`extraido/`),
  builds (`build/`, `.app`, `.ipa`).

`.gitignore` blocks all of these. Every player supplies their own legally obtained copy of the game, and the
translation and build happen on their own machine.

## Nintendo

Mario Kart DS, its characters, names, music, artwork and code are the property of Nintendo. This project is not
affiliated with, endorsed or sponsored by Nintendo. Character names that appear in the interface are only labels
for the game's own characters, which come from the player's copy of the game.

## Distribution

There is no public build: no IPA, no app on any store, no TestFlight, no release downloads. Builds contain code
translated from the player's own copy of the game and must not be shared.
