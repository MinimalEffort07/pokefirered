-------------------------------------------------------------------------------
-- test_all_characters_quick.lua
-------------------------------------------------------------------------------
-- Exhaustive per-entry test of the full 105-character selection list.
--
-- Uses cs.test_character_quick — for each entry we boot, open the list,
-- scroll to its index, press A, and read back playerAvatarGfxId. We skip
-- the overworld/walk verification (~6k frames/char) so this finishes in a
-- few minutes rather than half an hour.
--
-- The goal is to detect list-mapping bugs: entries where selecting the
-- character stores the wrong graphicsId (or fails to register at all).
-- Once a bug is localized, run test_playable_characters.lua on the specific
-- name to get the full overworld + walk verification.
--
-- HOW TO RUN (needs a long timeout — headless is still real emulation):
--   bash test/run_test.sh test/tests/test_all_characters_quick.lua
--   ...or invoke mgba-headless directly with a larger `timeout`.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

-- All 105 entries from sCharacterListItems in src/oak_speech.c.
-- Format: { list_index, expected_gfx_id, display_name }
-- Keep this in sync with the source file if the list changes.
local CHARACTERS = {
    {   0,   0, "RED"                    },
    {   1,   7, "GREEN"                  },
    {   2,  14, "BRENDAN"                },
    {   3,  15, "MAY"                    },
    {   4,  16, "LITTLE_BOY"             },
    {   5,  17, "LITTLE_GIRL"            },
    {   6,  18, "YOUNGSTER"              },
    {   7,  19, "BOY"                    },
    {   8,  20, "BUG_CATCHER"            },
    {   9,  22, "LASS"                   },
    {  10,  23, "WOMAN_1"                },
    {  11,  24, "BATTLE_GIRL"            },
    {  12,  25, "MAN"                    },
    {  13,  26, "ROCKER"                 },
    {  14,  27, "FAT_MAN"                },
    {  15,  28, "WOMAN_2"                },
    {  16,  29, "BEAUTY"                 },
    {  17,  30, "BALDING_MAN"            },
    {  18,  31, "WOMAN_3"                },
    {  19,  32, "OLD_MAN_1"              },
    {  20,  33, "OLD_MAN_2"              },
    {  21,  35, "OLD_WOMAN"              },
    {  22,  36, "TUBER_M_WATER"          },
    {  23,  37, "TUBER_F"                },
    {  24,  38, "TUBER_M_LAND"           },
    {  25,  39, "CAMPER"                 },
    {  26,  40, "PICNICKER"              },
    {  27,  41, "COOLTRAINER_M"          },
    {  28,  42, "COOLTRAINER_F"          },
    {  29,  43, "SWIMMER_M_WATER"        },
    {  30,  44, "SWIMMER_F_WATER"        },
    {  31,  45, "SWIMMER_M_LAND"         },
    {  32,  46, "SWIMMER_F_LAND"         },
    {  33,  47, "WORKER_M"               },
    {  34,  48, "WORKER_F"               },
    {  35,  49, "ROCKET_M"               },
    {  36,  50, "ROCKET_F"               },
    {  37,  51, "GBA_KID"                },
    {  38,  52, "SUPER_NERD"             },
    {  39,  53, "BIKER"                  },
    {  40,  54, "BLACKBELT"              },
    {  41,  55, "SCIENTIST"              },
    {  42,  56, "HIKER"                  },
    {  43,  57, "FISHER"                 },
    {  44,  58, "CHANNELER"              },
    {  45,  59, "CHEF"                   },
    {  46,  60, "POLICEMAN"              },
    {  47,  61, "GENTLEMAN"              },
    {  48,  62, "SAILOR"                 },
    {  49,  63, "CAPTAIN"                },
    {  50,  64, "NURSE"                  },
    {  51,  65, "CABLE_CLUB_RECEPTIONIST"},
    {  52,  66, "UNION_ROOM_RECEPTIONIST"},
    {  53,  67, "UNUSED_MALE_RECEPTIONIST"},
    {  54,  68, "CLERK"                  },
    {  55,  69, "MG_DELIVERYMAN"         },
    {  56,  70, "TRAINER_TOWER_DUDE"     },
    {  57,  71, "PROF_OAK"               },
    {  58,  72, "BLUE"                   },
    {  59,  73, "BILL"                   },
    {  60,  74, "LANCE"                  },
    {  61,  75, "AGATHA"                 },
    {  62,  76, "DAISY"                  },
    {  63,  77, "LORELEI"                },
    {  64,  78, "MR_FUJI"                },
    {  65,  79, "BRUNO"                  },
    {  66,  80, "BROCK"                  },
    {  67,  81, "MISTY"                  },
    {  68,  82, "LT_SURGE"               },
    {  69,  83, "ERIKA"                  },
    {  70,  84, "KOGA"                   },
    {  71,  85, "SABRINA"                },
    {  72,  86, "BLAINE"                 },
    {  73,  87, "GIOVANNI"               },
    {  74,  88, "MOM"                    },
    {  75,  89, "CELIO"                  },
    {  76,  90, "TEACHY_TV_HOST"         },
    {  77,  91, "GYM_GUY"                },
    {  78, 120, "PIKACHU"                },
    {  79, 110, "SPEAROW"                },
    {  80, 111, "CUBONE"                 },
    {  81, 112, "POLIWRATH"              },
    {  82, 113, "CLEFAIRY"               },
    {  83, 114, "PIDGEOT"                },
    {  84, 115, "JIGGLYPUFF"             },
    {  85, 116, "PIDGEY"                 },
    {  86, 117, "CHANSEY"                },
    {  87, 118, "OMANYTE"                },
    {  88, 119, "KANGASKHAN"             },
    {  89, 121, "PSYDUCK"                },
    {  90, 122, "NIDORAN_F"              },
    {  91, 123, "NIDORAN_M"              },
    {  92, 124, "NIDORINO"               },
    {  93, 125, "MEOWTH"                 },
    {  94, 126, "SEEL"                   },
    {  95, 127, "VOLTORB"                },
    {  96, 128, "SLOWPOKE"               },
    {  97, 129, "SLOWBRO"                },
    {  98, 130, "MACHOP"                 },
    {  99, 131, "WIGGLYTUFF"             },
    { 100, 132, "DODUO"                  },
    { 101, 133, "FEAROW"                 },
    { 102, 134, "MACHOKE"                },
    { 103, 135, "LAPRAS"                 },
    { 104, 147, "KABUTO"                 },
}

fw.run(function()
    fw.log(string.format("=== Quick test: all %d characters ===", #CHARACTERS))

    local all_results = {}
    for _, char in ipairs(CHARACTERS) do
        local r = cs.test_character_quick(char[1], char[2], char[3])
        table.insert(all_results, r)
    end

    fw.log("=== Summary ===")
    local failed = {}
    for _, r in ipairs(all_results) do
        if not r.passed then
            fw.log_error(string.format("  FAIL idx=%-3d %-26s (got gfx=%s, expected %d): %s",
                r.list_index, r.name, tostring(r.gfx_saved), r.expected_gfx,
                table.concat(r.reasons, "; ")))
            table.insert(failed, string.format("%s(idx=%d)", r.name, r.list_index))
        end
    end

    for _, r in ipairs(all_results) do
        fw.assert_true(r.passed,
            string.format("idx=%d %s maps to gfx=%d",
                r.list_index, r.name, r.expected_gfx))
    end

    if #failed > 0 then
        fw.log_error(string.format("FAILED %d/%d: %s",
            #failed, #CHARACTERS, table.concat(failed, ", ")))
    else
        fw.log(string.format("All %d characters passed", #CHARACTERS))
    end

    fw.finish()
end)
