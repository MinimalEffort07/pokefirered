# Pokémon FireRed and LeafGreen

This is a fork of the PRET project with custom features. Development is AI-assisted (Claude Code) with human review and direction.

## Added Features

- **Real-Time 4-Player Multiplayer** — Multi-player position sync via GBA Serial I/O, with remote player sprites on shared maps.

- **Procedurally Generated Mt. Moon** — Every visit to Mt. Moon builds a new cave on each of its three floors.
  - **Chain:** Mt. Moon is a chain of three generated floors: enter from Route 4's west door into 1F, climb down a ladder to B1F, down again to B2F, and climb a ladder out of B2F to Route 4's east door, or walk it the other way. Each floor has exactly two ways out, one toward each end of the chain, and they are at least 24 tiles apart (east-west plus north-south). 1F keeps vanilla's doorway at the bottom of the map, and the ladders sit the way vanilla Mt. Moon places them: a ladder down is a hole in open floor, a ladder up stands at the foot of a north wall.
  - **Saves:** Every visit rolls a new cave seed, stored in the save, and the whole cave is built from that seed alone, so a cave survives save and reload: Continue puts you back in the same cave, on the same tile, with every trainer and item where you left them. If a save's tile isn't walkable in the rebuilt cave (for example a save made before seeds existed), you are moved to the nearest reachable floor.
  - **Content:** Every trainer, NPC, item ball, hidden item and sign of vanilla Mt. Moon is on its own floor, and all of it is guaranteed reachable: from either ladder you can walk to the other ladder, to a tile facing every trainer, NPC, item, hidden item and sign, and to every floor tile of the cave. No object ever walls off part of a cave, not even a defeated trainer, who never moves again. Hidden items sit on a lone boulder as in vanilla, and trainers that stand still face down their longest straight corridor.
  - **Load time:** Entering a floor takes at most half a second longer than entering a vanilla cave of the same size.
  - **1F look:** 1F currently uses the shared cellular-automata cave style.
  - **B1F look:** B1F looks like vanilla B1F: rectangular rooms and corridors carved out of black void, each with a 2-row rock wall along its north side, and about a fifth of the map walkable (vanilla: 16%). Rooms at different heights join the way vanilla's do, with rubble where a lower wall ends beside a corridor. Every tile sits only next to tiles vanilla B1F also puts beside it, so there are no visible seams. The ladder up stands at the foot of a north wall and the hole down is in open floor.
  - **B2F look:** B2F currently uses the shared cellular-automata cave style.
  - **Fossil chamber:** Not built yet: Super Nerd Miguel and the two fossils are placed on B2F like any other object, and Miguel's battle trigger stays on vanilla's tile (14,11).
  - **Decorations:** None yet: no rubble, craters, puddles or stairs.

  ![Procedurally Generated Mt. Moon](gifs/procedurallyGenerated.gif)

- **Quick Select Menu** — Press SELECT in the overworld to use HMs directly and access registered items from a single menu.

  ![Quick Select Menu](gifs/quickselect.gif)

- **Character Select** — Choose your player character at the start of the game.

  ![Character Select](gifs/character_select.gif)

- **Cycle Anywhere** — Use the bicycle anywhere without restrictions.

  ![Cycle Anywhere](gifs/cycleAnywhere.gif)

- **Type Effectiveness Colors in Battle** — Move text in the FIGHT menu is colored by effectiveness: green for super effective, yellow for not very effective, red for no effect.

  ![Type Effectiveness Colors](gifs/ColourizedMoves.gif)

- **Walk With Pokemon** — Your pokemon follows you around in the overworld.

  ![Walk With Pokemon](gifs/WalkWithPokemon.gif)

- **Shiny Chaining (Gen 4 PokéRadar)** — Granted at game start. Use in tall grass to spawn shaking patches; defeating or catching the same species from patches builds a chain, boosting shiny odds up to ~1/200 at chain 40.

  ![Shiny Chaining](gifs/shinyChaining.gif)

- **PP Items in Marts** — Ether, Max Ether, Elixir, and Max Elixir are now sold in Pokémon Marts, tiered to match the HP restorers already available: Ether/Max Ether alongside Super Potions, Elixir alongside Hyper Potions, Max Elixir alongside Max Potions.

  ![PP Items in Marts](gifs/pp_items_in_marts.gif)

- **PC Access from Anywhere** — "MOVE TO PC" option in the party menu deposits a Pokémon directly to the PC box. "BILL'S PC" in the Quick Select menu (SELECT button) opens the full PC storage system from anywhere outside battle.

  ![PC Access from Anywhere](gifs/pc_anywhere.gif)

- **L/R Page Scroll in Bag and Pokédex** — Press L or R while in the Bag item list or the Pokédex ordered list to page the cursor up or down by a full screen at a time. L clamps at the top of the list (no wrap-around).

  ![L/R Page Scroll](gifs/lr_page_scroll.gif)


This is a decompilation of English Pokémon FireRed and LeafGreen.

It builds the following ROM images:

* [**pokefirered.gba**](https://datomatic.no-intro.org/?page=show_record&s=23&n=1616) `sha1: 41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc`
* [**pokeleafgreen.gba**](https://datomatic.no-intro.org/?page=show_record&s=23&n=1617) `sha1: 574fa542ffebb14be69902d1d36f1ec0a4afd71e`
* [**pokefirered_rev1.gba**](https://datomatic.no-intro.org/?page=show_record&s=23&n=1672) `sha1: dd5945db9b930750cb39d00c84da8571feebf417`
* [**pokeleafgreen_rev1.gba**](https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=1668) `sha1: 7862c67bdecbe21d1d69ce082ce34327e1c6ed5e`
* [**pokefirered_switch.gba**](https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=x550) `sha1: baa452d0b24629dd7782cfc07a8984085dde1311`
* [**pokeleafgreen_switch.gba**](https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=x551) `sha1: 62b9fc77549dbc67032eb6cbd0ea6ad3b825690f`

To set up the repository, see [INSTALL.md](INSTALL.md).

For contacts and other pret projects, see [pret.github.io](https://pret.github.io/).
