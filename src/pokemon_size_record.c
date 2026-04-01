/**
 * =POKEMON SIZE RECORD SYSTEM=
 *
 * FILE OVERVIEW:
 * This file implements the Pokemon size-comparison minigame found in the
 * game world (e.g., the Magikarp fishing guru, Heracross size contests).
 * NPCs challenge the player to find the biggest specimen of a particular
 * species and track the record size.
 *
 * SIZE CALCULATION:
 * A Pokemon's size is determined by a hash computed from its personality
 * value and IVs (Individual Values). This hash indexes into a size
 * distribution table (sBigMonSizeTable) to get a multiplier, which is
 * then applied to the species' Pokedex height. This means two Magikarp
 * with different personalities/IVs will have different sizes, creating
 * a natural variation that makes the contest interesting.
 *
 * The distribution is bell-curve-shaped: most Pokemon are near average
 * size, with very large or very small specimens being rare.
 *
 * This file also includes a function for giving Gift Ribbons to the
 * player's party — an unrelated feature that was grouped here in the
 * original codebase.
 */
#include "global.h"
#include "gflib.h"
#include "data.h"
#include "event_data.h"
#include "pokedex.h"
#include "text.h"
#include "strings.h"

/* Default starting record size — 0 means no record set yet.
 * Ruby/Sapphire used 0x8100 as a default seed value, but FireRed uses 0. */
#define DEFAULT_MAX_SIZE 0

/**
 * Size distribution table entry.
 * unk0 = size multiplier (in tenths — 1000 = 100.0% = species' default height)
 * unk2 = range width (how many hash values map to this size bracket)
 * unk4 = cumulative starting offset (sum of all previous ranges)
 *
 * The table creates a bell curve: most hash values map to sizes near
 * 700-1000 (70-100% of Pokedex height), with extreme sizes being rare.
 */
struct UnknownStruct
{
    u16 unk0;   /* Size multiplier in tenths of percent */
    u8 unk2;    /* Range width for this bracket */
    u16 unk4;   /* Cumulative offset (hash threshold) */
};

static const struct UnknownStruct sBigMonSizeTable[] =
{
    {  290,   1,      0 },
    {  300,   1,     10 },
    {  400,   2,    110 },
    {  500,   4,    310 },
    {  600,  20,    710 },
    {  700,  50,   2710 },
    {  800, 100,   7710 },
    {  900, 150,  17710 },
    { 1000, 150,  32710 },
    { 1100, 100,  47710 },
    { 1200,  50,  57710 },
    { 1300,  20,  62710 },
    { 1400,   5,  64710 },
    { 1500,   2,  65210 },
    { 1600,   1,  65410 },
    { 1700,   1,  65510 },
};

static const u8 sGiftRibbonsMonDataIds[] =
{
    MON_DATA_MARINE_RIBBON, MON_DATA_LAND_RIBBON, MON_DATA_SKY_RIBBON,
    MON_DATA_COUNTRY_RIBBON, MON_DATA_NATIONAL_RIBBON, MON_DATA_EARTH_RIBBON,
    MON_DATA_WORLD_RIBBON
};

#define CM_PER_INCH 2.54

/**
 * FUNCTION: GetMonSizeHash
 *
 * PURPOSE: Generates a 16-bit hash from a Pokemon's personality and IVs
 * that determines its unique size. Two Pokemon of the same species with
 * different stats will have different sizes.
 *
 * HOW IT WORKS:
 * Takes the low 4 bits of each IV (0-15) and combines them with the
 * personality value using XOR and multiplication:
 *   High byte: (AtkIV ^ DefIV) * HpIV, XORed with personality low byte
 *   Low byte:  (SpAIV ^ SpDIV) * SpdIV, XORed with personality high byte
 * This produces a pseudo-random 16-bit value that's deterministic for
 * any given Pokemon (same Pokemon always gets the same hash).
 */
static u32 GetMonSizeHash(struct Pokemon * pkmn)
{
    u16 personality = GetMonData(pkmn, MON_DATA_PERSONALITY);
    u16 hpIV = GetMonData(pkmn, MON_DATA_HP_IV) & 0xF;
    u16 attackIV = GetMonData(pkmn, MON_DATA_ATK_IV) & 0xF;
    u16 defenseIV = GetMonData(pkmn, MON_DATA_DEF_IV) & 0xF;
    u16 speedIV = GetMonData(pkmn, MON_DATA_SPEED_IV) & 0xF;
    u16 spAtkIV = GetMonData(pkmn, MON_DATA_SPATK_IV) & 0xF;
    u16 spDefIV = GetMonData(pkmn, MON_DATA_SPDEF_IV) & 0xF;
    u32 hibyte = ((attackIV ^ defenseIV) * hpIV) ^ (personality & 0xFF);
    u32 lobyte = ((spAtkIV ^ spDefIV) * speedIV) ^ (personality >> 8);

    return (hibyte << 8) + lobyte;
}

static u8 TranslateBigMonSizeTableIndex(u16 a)
{
    u8 i;

    for (i = 1; i < 15; i++)
    {
        if (a < sBigMonSizeTable[i].unk4)
            return i - 1;
    }
    return i;
}

static u32 GetMonSize(u16 species, u16 b)
{
    u64 unk2;
    u64 unk4;
    u64 unk0;
    u32 height;
    u32 var;

    height = GetPokedexHeightWeight(SpeciesToNationalPokedexNum(species), 0);
    var = TranslateBigMonSizeTableIndex(b);
    unk0 = sBigMonSizeTable[var].unk0;
    unk2 = sBigMonSizeTable[var].unk2;
    unk4 = sBigMonSizeTable[var].unk4;
    unk0 += (b - unk4) / unk2;
    return height * unk0 / 10;
}

static void FormatMonSizeRecord(u8 *string, u32 size)
{
#ifdef UNITS_IMPERIAL
    //Convert size from centimeters to inches
    //In the Hoenn games, this conversion was performed using floating point values
    size = size * 100 / 254;
#endif

    string = ConvertIntToDecimalStringN(string, size / 10, STR_CONV_MODE_LEFT_ALIGN, 8);
    string = StringAppend(string, gText_DecimalPoint);
    ConvertIntToDecimalStringN(string, size % 10, STR_CONV_MODE_LEFT_ALIGN, 1);
}

static u8 CompareMonSize(u16 species, u16 *sizeRecord)
{
    if (gSpecialVar_Result >= PARTY_SIZE)
    {
        return 0;
    }
    else
    {
        struct Pokemon * pkmn = &gPlayerParty[gSpecialVar_Result];

        if (GetMonData(pkmn, MON_DATA_IS_EGG) == TRUE || GetMonData(pkmn, MON_DATA_SPECIES) != species)
        {
            return 1;
        }
        else
        {
            u32 oldSize;
            u32 newSize;
            u16 sizeParams;

            *(&sizeParams) = GetMonSizeHash(pkmn);
            newSize = GetMonSize(species, sizeParams);
            oldSize = GetMonSize(species, *sizeRecord);
            FormatMonSizeRecord(gStringVar3, oldSize);
            FormatMonSizeRecord(gStringVar2, newSize);
            if (newSize == oldSize)
            {
                return 4;
            }
            else if (newSize < oldSize)
            {
                return 2;
            }
            else
            {
                *sizeRecord = sizeParams;
                return 3;
            }
        }
    }
}

// Stores species name in gStringVar1, trainer's name in gStringVar2, and size in gStringVar3
static void GetMonSizeRecordInfo(u16 species, u16 *sizeRecord)
{
    u32 size = GetMonSize(species, *sizeRecord);

    FormatMonSizeRecord(gStringVar3, size);
    StringCopy(gStringVar1, gSpeciesNames[species]);
}

void InitHeracrossSizeRecord(void)
{
    VarSet(VAR_HERACROSS_SIZE_RECORD, DEFAULT_MAX_SIZE);
}

void GetHeracrossSizeRecordInfo(void)
{
    u16 *sizeRecord = GetVarPointer(VAR_HERACROSS_SIZE_RECORD);

    GetMonSizeRecordInfo(SPECIES_HERACROSS, sizeRecord);
}

void CompareHeracrossSize(void)
{
    u16 *sizeRecord = GetVarPointer(VAR_HERACROSS_SIZE_RECORD);

    gSpecialVar_Result = CompareMonSize(SPECIES_HERACROSS, sizeRecord);
}

void InitMagikarpSizeRecord(void)
{
    VarSet(VAR_MAGIKARP_SIZE_RECORD, DEFAULT_MAX_SIZE);
}

void GetMagikarpSizeRecordInfo(void)
{
    u16 *sizeRecord = GetVarPointer(VAR_MAGIKARP_SIZE_RECORD);

    GetMonSizeRecordInfo(SPECIES_MAGIKARP, sizeRecord);
}

void CompareMagikarpSize(void)
{
    u16 *sizeRecord = GetVarPointer(VAR_MAGIKARP_SIZE_RECORD);

    gSpecialVar_Result = CompareMonSize(SPECIES_MAGIKARP, sizeRecord);
}

/**
 * FUNCTION: GiveGiftRibbonToParty
 *
 * PURPOSE: Awards a special Gift Ribbon to every Pokemon in the player's
 * party. Gift Ribbons are special event ribbons (Marine, Land, Sky, etc.)
 * distributed at real-world Pokemon events.
 *
 * @param index — which ribbon slot to set (0-10, maps to sGiftRibbonsMonDataIds)
 * @param ribbonId — the ribbon ID value to store in the save data
 */
void GiveGiftRibbonToParty(u8 index, u8 ribbonId)
{
    s32 i;
    bool32 gotRibbon = FALSE;
    u8 data = 1;
    u8 array[8];
    memcpy(array, sGiftRibbonsMonDataIds, sizeof(sGiftRibbonsMonDataIds));

    if (index < 11 && ribbonId < 65)
    {
        gSaveBlock1Ptr->giftRibbons[index] = ribbonId;
        for (i = 0; i < PARTY_SIZE; i++)
        {
            struct Pokemon * mon = &gPlayerParty[i];

            if (GetMonData(mon, MON_DATA_SPECIES) != SPECIES_NONE && !GetMonData(mon, MON_DATA_SANITY_IS_EGG))
            {
                SetMonData(mon, array[index], &data);
                gotRibbon = TRUE;
            }
        }
        if (gotRibbon)
            FlagSet(FLAG_SYS_RIBBON_GET);
    }
}
