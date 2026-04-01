/*
 * script.c - Event Script Engine
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 *
 * This file implements the GBA Pokemon game's scripting engine. Event scripts
 * control virtually all in-game events: NPC dialogue, item pickups, trainer
 * battles, cutscenes, map warps, and story progression.
 *
 * ARCHITECTURE:
 * Scripts are bytecode programs stored in ROM. Each byte is a command opcode,
 * followed by parameters (1/2/4 bytes depending on the command). The engine
 * reads opcodes one at a time, looks up the handler function in gScriptCmdTable[],
 * and calls it. Handlers return TRUE to yield (wait for user input, animation,
 * etc.) or FALSE to continue to the next command immediately.
 *
 * SCRIPT CONTEXTS:
 * There are two script contexts:
 *   1. sGlobalScriptContext: The primary context. Runs one command per frame,
 *      yielding back to the main game loop between commands. Used for NPC
 *      dialogue, trainer encounters, and most scripted events.
 *   2. sImmediateScriptContext: Runs to completion in a single frame (blocking).
 *      Used for map header scripts (on-load, on-transition, etc.) that must
 *      finish before the map is displayed.
 *
 * CALL STACK:
 * Scripts support subroutine calls (ScriptCall/ScriptReturn) with a 20-level
 * stack. The "call" command pushes the return address, and "return" pops it.
 * This allows reusable script snippets.
 *
 * NATIVE CALLBACKS:
 * Scripts can switch to "native mode" where a C function runs instead of
 * bytecode. When the C function returns TRUE, execution switches back to
 * bytecode mode. This is used for complex operations like menu systems.
 *
 * RAM SCRIPTS:
 * Special scripts that are stored in save RAM rather than ROM. Used by the
 * Mystery Gift system to inject custom event scripts into the game.
 *
 * MAP SCRIPTS:
 * Each map header can define several script types:
 *   ON_LOAD: Runs when map tiles are loaded (set metatiles, etc.)
 *   ON_TRANSITION: Runs during map transition (set weather, music, etc.)
 *   ON_RESUME: Runs when returning to this map
 *   ON_FRAME_TABLE: Conditional scripts checked every frame (trigger events)
 *   ON_WARP_INTO_MAP_TABLE: Conditional scripts checked on warp arrival
 * ============================================================================
 */

#include "global.h"
#include "script.h"
#include "event_data.h"
#include "quest_log.h"
#include "mystery_gift.h"
#include "constants/maps.h"
#include "constants/map_scripts.h"

extern void ResetContextNpcTextColor(void); // field_specials
extern u16 CalcCRC16WithTable(u8 *data, int length); // util

/* Magic number that marks a valid RAM script in save data */
#define RAM_SCRIPT_MAGIC 51

/*
 * Script execution modes:
 * STOPPED: Script is inactive, not running
 * BYTECODE: Reading and executing bytecode opcodes from scriptPtr
 * NATIVE: Calling a C function pointer (nativePtr) each frame
 */
enum {
    SCRIPT_MODE_STOPPED,
    SCRIPT_MODE_BYTECODE,
    SCRIPT_MODE_NATIVE,
};

/*
 * Global script context states:
 * RUNNING: Script is actively executing (runs one command per frame tick)
 * WAITING: Script paused, waiting for something (menu, animation, etc.)
 * SHUTDOWN: Script has finished or hasn't been started
 */
enum {
    CONTEXT_RUNNING,
    CONTEXT_WAITING,
    CONTEXT_SHUTDOWN,
};

EWRAM_DATA u8 gWalkAwayFromSignInhibitTimer = 0;
EWRAM_DATA const u8 *gRamScriptRetAddr = NULL;

static u8 sGlobalScriptContextStatus;
static u32 sUnusedVariable1;
static struct ScriptContext sGlobalScriptContext;
static u32 sUnusedVariable2;
static struct ScriptContext sImmediateScriptContext;
static bool8 sLockFieldControls;
static u8 sMsgBoxWalkawayDisabled;
static u8 sMsgBoxIsCancelable;
static u8 sQuestLogInput;
static u8 sQuestLogInputIsDpad;
static u8 sMsgIsSignpost;

extern ScrCmdFunc gScriptCmdTable[];
extern ScrCmdFunc gScriptCmdTableEnd[];
extern void *gNullScriptPtr;

/**
 * FUNCTION: InitScriptContext
 *
 * PURPOSE: Reset a script context to its initial empty state.
 *
 * @param ctx — The script context to initialize
 * @param cmdTable — Pointer to the command function table (gScriptCmdTable)
 * @param cmdTableEnd — Pointer past the end of the table (for bounds checking)
 */
void InitScriptContext(struct ScriptContext *ctx, void *cmdTable, void *cmdTableEnd)
{
    s32 i;

    ctx->mode = SCRIPT_MODE_STOPPED;
    ctx->scriptPtr = NULL;
    ctx->stackDepth = 0;
    ctx->nativePtr = NULL;
    ctx->cmdTable = cmdTable;
    ctx->cmdTableEnd = cmdTableEnd;

    for (i = 0; i < (int)ARRAY_COUNT(ctx->data); i++)
        ctx->data[i] = 0;

    for (i = 0; i < (int)ARRAY_COUNT(ctx->stack); i++)
        ctx->stack[i] = 0;
}

u8 SetupBytecodeScript(struct ScriptContext *ctx, const u8 *ptr)
{
    ctx->scriptPtr = ptr;
    ctx->mode = SCRIPT_MODE_BYTECODE;
    return 1;
}

void SetupNativeScript(struct ScriptContext *ctx, bool8 (*ptr)(void))
{
    ctx->mode = SCRIPT_MODE_NATIVE;
    ctx->nativePtr = ptr;
}

void StopScript(struct ScriptContext *ctx)
{
    ctx->mode = SCRIPT_MODE_STOPPED;
    ctx->scriptPtr = NULL;
}

/**
 * FUNCTION: RunScriptCommand
 *
 * PURPOSE: Execute the next script command(s) in the given context.
 *
 * HOW IT WORKS:
 * In NATIVE mode: calls the C function. If it returns TRUE, switches to
 * BYTECODE mode (the native callback has finished its work).
 *
 * In BYTECODE mode: reads one opcode byte from scriptPtr, looks it up in
 * cmdTable, and calls the handler. If the handler returns TRUE, this
 * function yields (returns TRUE to caller). If the handler returns FALSE,
 * it continues to the next command immediately (tight loop). This allows
 * commands like "set variable" to execute without consuming a frame.
 *
 * Safety: if scriptPtr is NULL, the script stops. If it equals gNullScriptPtr
 * (a special poison value), the game HALTS -- this catches dangling pointers.
 *
 * @return TRUE if there's more script to run, FALSE if done
 */
bool8 RunScriptCommand(struct ScriptContext *ctx)
{
    // FRLG disabled this check, where-as it is present
    // in Ruby/Sapphire and Emerald. Why did the programmers
    // bother to remove a redundant check when it still
    // exists in Emerald?
    //if (ctx->mode == SCRIPT_MODE_STOPPED)
    //    return FALSE;

    switch (ctx->mode)
    {
    case SCRIPT_MODE_STOPPED:
        return FALSE;
    case SCRIPT_MODE_NATIVE:
        // Try to call a function in C
        // Continue to bytecode if no function or it returns TRUE
        if (ctx->nativePtr)
        {
            if (ctx->nativePtr() == TRUE)
                ctx->mode = SCRIPT_MODE_BYTECODE;
            return TRUE;
        }
        ctx->mode = SCRIPT_MODE_BYTECODE;
        // fallthrough
    case SCRIPT_MODE_BYTECODE:
        while (1)
        {
            u8 cmdCode;
            ScrCmdFunc *cmdFunc;

            if (ctx->scriptPtr == NULL)
            {
                ctx->mode = SCRIPT_MODE_STOPPED;
                return FALSE;
            }

            if (ctx->scriptPtr == gNullScriptPtr)
            {
                while (1)
                    asm("svc 2"); // HALT
            }

            cmdCode = *(ctx->scriptPtr);
            ctx->scriptPtr++;
            cmdFunc = &ctx->cmdTable[cmdCode];

            if (cmdFunc >= ctx->cmdTableEnd)
            {
                ctx->mode = SCRIPT_MODE_STOPPED;
                return FALSE;
            }

            if ((*cmdFunc)(ctx) == TRUE)
                return TRUE;
        }
    }

    return TRUE;
}

static u8 ScriptPush(struct ScriptContext *ctx, const u8 *ptr)
{
    if (ctx->stackDepth + 1 >= (int)ARRAY_COUNT(ctx->stack))
    {
        return 1;
    }
    else
    {
        ctx->stack[ctx->stackDepth] = ptr;
        ctx->stackDepth++;
        return 0;
    }
}

static const u8 *ScriptPop(struct ScriptContext *ctx)
{
    if (ctx->stackDepth == 0)
        return NULL;

    ctx->stackDepth--;
    return ctx->stack[ctx->stackDepth];
}

void ScriptJump(struct ScriptContext *ctx, const u8 *ptr)
{
    ctx->scriptPtr = ptr;
}

void ScriptCall(struct ScriptContext *ctx, const u8 *ptr)
{
    ScriptPush(ctx, ctx->scriptPtr);
    ctx->scriptPtr = ptr;
}

void ScriptReturn(struct ScriptContext *ctx)
{
    ctx->scriptPtr = ScriptPop(ctx);
}

/**
 * FUNCTION: ScriptReadHalfword
 *
 * PURPOSE: Read a 16-bit value from the script bytecode stream.
 *
 * HOW IT WORKS:
 * GBA uses little-endian byte order, and the ARM7TDMI can handle unaligned
 * 8-bit reads but not unaligned 16-bit reads. So we read two bytes separately
 * and combine them: low byte first, high byte second (little-endian).
 * Advances scriptPtr by 2 bytes.
 */
u16 ScriptReadHalfword(struct ScriptContext *ctx)
{
    u16 value = *(ctx->scriptPtr++);
    value |= *(ctx->scriptPtr++) << 8;
    return value;
}

u32 ScriptReadWord(struct ScriptContext *ctx)
{
    u32 value0 = *(ctx->scriptPtr++);
    u32 value1 = *(ctx->scriptPtr++);
    u32 value2 = *(ctx->scriptPtr++);
    u32 value3 = *(ctx->scriptPtr++);
    return (((((value3 << 8) + value2) << 8) + value1) << 8) + value0;
}

void LockPlayerFieldControls(void)
{
    sLockFieldControls = TRUE;
}

void UnlockPlayerFieldControls(void)
{
    sLockFieldControls = FALSE;
}

bool8 ArePlayerFieldControlsLocked(void)
{
    return sLockFieldControls;
}

void SetQuestLogInputIsDpadFlag(void)
{
    sQuestLogInputIsDpad = TRUE;
}

void ClearQuestLogInputIsDpadFlag(void)
{
    sQuestLogInputIsDpad = FALSE;
}

bool8 IsQuestLogInputDpad(void)
{
    if(sQuestLogInputIsDpad == TRUE)
        return TRUE;
    else
        return FALSE;
}

void RegisterQuestLogInput(u8 var)
{
    sQuestLogInput = var;
}

void ClearQuestLogInput(void)
{
    sQuestLogInput = 0;
}

u8 GetRegisteredQuestLogInput(void)
{
    return sQuestLogInput;
}

void DisableMsgBoxWalkaway(void)
{
    sMsgBoxWalkawayDisabled = TRUE;
}

void EnableMsgBoxWalkaway(void)
{
    sMsgBoxWalkawayDisabled = FALSE;
}

bool8 IsMsgBoxWalkawayDisabled(void)
{
    return sMsgBoxWalkawayDisabled;
}

void SetWalkingIntoSignVars(void)
{
    gWalkAwayFromSignInhibitTimer = 6;
    sMsgBoxIsCancelable = TRUE;
}

void ClearMsgBoxCancelableState(void)
{
    sMsgBoxIsCancelable = FALSE;
}

bool8 CanWalkAwayToCancelMsgBox(void)
{
    if(sMsgBoxIsCancelable == TRUE)
        return TRUE;
    else
        return FALSE;
}

void MsgSetSignpost(void)
{
    sMsgIsSignpost = TRUE;
}

void MsgSetNotSignpost(void)
{
    sMsgIsSignpost = FALSE;
}

bool8 IsMsgSignpost(void)
{
    if(sMsgIsSignpost == TRUE)
        return TRUE;
    else
        return FALSE;
}

void ResetFacingNpcOrSignpostVars(void)
{
    ResetContextNpcTextColor();
    MsgSetNotSignpost();
}

// The ScriptContext_* functions work with the primary script context,
// which yields control back to native code should the script make a wait call.

// Checks if the global script context is able to be run right now.
bool8 ScriptContext_IsEnabled(void)
{
    if (sGlobalScriptContextStatus == CONTEXT_RUNNING)
        return TRUE;
    else
        return FALSE;
}

// Re-initializes the global script context to zero.
void ScriptContext_Init(void)
{
    InitScriptContext(&sGlobalScriptContext, gScriptCmdTable, gScriptCmdTableEnd);
    sGlobalScriptContextStatus = CONTEXT_SHUTDOWN;
}

// Runs the script until the script makes a wait* call, then returns true if 
// there's more script to run, or false if the script has hit the end. 
// This function also returns false if the context is finished 
// or waiting (after a call to _Stop)
bool8 ScriptContext_RunScript(void)
{
    if (sGlobalScriptContextStatus == CONTEXT_SHUTDOWN)
        return FALSE;

    if (sGlobalScriptContextStatus == CONTEXT_WAITING)
        return FALSE;

    LockPlayerFieldControls();

    if (!RunScriptCommand(&sGlobalScriptContext))
    {
        sGlobalScriptContextStatus = CONTEXT_SHUTDOWN;
        UnlockPlayerFieldControls();
        return FALSE;
    }

    return TRUE;
}

// Sets up a new script in the global context and enables the context
void ScriptContext_SetupScript(const u8 *ptr)
{
    ClearMsgBoxCancelableState();
    EnableMsgBoxWalkaway();
    ClearQuestLogInputIsDpadFlag();

    InitScriptContext(&sGlobalScriptContext, gScriptCmdTable, gScriptCmdTableEnd);
    SetupBytecodeScript(&sGlobalScriptContext, ptr);
    LockPlayerFieldControls();
    sGlobalScriptContextStatus = CONTEXT_RUNNING;
}

// Puts the script into waiting mode; usually called from a wait* script command.
void ScriptContext_Stop(void)
{
    sGlobalScriptContextStatus = CONTEXT_WAITING;
}

// Puts the script into running mode.
void ScriptContext_Enable(void)
{
    sGlobalScriptContextStatus = CONTEXT_RUNNING;
    LockPlayerFieldControls();
}

// Sets up and runs a script in its own context immediately. The script will be
// finished when this function returns. Used mainly by all of the map header
// scripts (except the frame table scripts).
void RunScriptImmediately(const u8 *ptr)
{
    InitScriptContext(&sImmediateScriptContext, &gScriptCmdTable, &gScriptCmdTableEnd);
    SetupBytecodeScript(&sImmediateScriptContext, ptr);
    while (RunScriptCommand(&sImmediateScriptContext) == TRUE);
}

static u8 *MapHeaderGetScriptTable(u8 tag)
{
    const u8 *mapScripts = gMapHeader.mapScripts;

    if (mapScripts == NULL)
        return NULL;

    while (1)
    {
        if (*mapScripts == 0)
            return NULL;
        if (*mapScripts == tag)
        {
            mapScripts++;
            return T2_READ_PTR(mapScripts);
        }
        mapScripts += 5;
    }
}

static void MapHeaderRunScriptType(u8 tag)
{
    u8 *ptr = MapHeaderGetScriptTable(tag);
    if (ptr != NULL)
        RunScriptImmediately(ptr);
}

static u8 *MapHeaderCheckScriptTable(u8 tag)
{
    u8 *ptr = MapHeaderGetScriptTable(tag);

    if (ptr == NULL)
        return NULL;

    while (1)
    {
        u16 varIndex1;
        u16 varIndex2;

        // Read first var (or .2byte terminal value)
        varIndex1 = T1_READ_16(ptr);
        if (!varIndex1)
            return NULL; // Reached end of table
        ptr += 2;

        // Read second var
        varIndex2 = T1_READ_16(ptr);
        ptr += 2;

        // Run map script if vars are equal
        if (VarGet(varIndex1) == VarGet(varIndex2))
            return T2_READ_PTR(ptr);
        ptr += 4;
    }
}

void RunOnLoadMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_LOAD);
}

void RunOnTransitionMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_TRANSITION);
}

void RunOnResumeMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_RESUME);
}

void RunOnReturnToFieldMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_RETURN_TO_FIELD);
}

void RunOnDiveWarpMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_DIVE_WARP);
}

bool8 TryRunOnFrameMapScript(void)
{
    u8 *ptr;

    if (gQuestLogState == QL_STATE_PLAYBACK_LAST)
        return FALSE;

    ptr = MapHeaderCheckScriptTable(MAP_SCRIPT_ON_FRAME_TABLE);

    if (!ptr)
        return FALSE;

    ScriptContext_SetupScript(ptr);
    return TRUE;
}

void TryRunOnWarpIntoMapScript(void)
{
    u8 *ptr = MapHeaderCheckScriptTable(MAP_SCRIPT_ON_WARP_INTO_MAP_TABLE);
    if (ptr)
        RunScriptImmediately(ptr);
}

u32 CalculateRamScriptChecksum(void)
{
    return CalcCRC16WithTable((u8 *)(&gSaveBlock1Ptr->ramScript.data), sizeof(gSaveBlock1Ptr->ramScript.data));
}

void ClearRamScript(void)
{
    CpuFill32(0, &gSaveBlock1Ptr->ramScript, sizeof(struct RamScript));
}

bool8 InitRamScript(u8 *script, u16 scriptSize, u8 mapGroup, u8 mapNum, u8 objectId)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;

    ClearRamScript();

    if (scriptSize > sizeof(scriptData->script))
        return FALSE;

    scriptData->magic = RAM_SCRIPT_MAGIC;
    scriptData->mapGroup = mapGroup;
    scriptData->mapNum = mapNum;
    scriptData->objectId = objectId;
    memcpy(scriptData->script, script, scriptSize);
    gSaveBlock1Ptr->ramScript.checksum = CalculateRamScriptChecksum();
    return TRUE;
}

const u8 *GetRamScript(u8 objectId, const u8 *script)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    gRamScriptRetAddr = NULL;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return script;
    if (scriptData->mapGroup != gSaveBlock1Ptr->location.mapGroup)
        return script;
    if (scriptData->mapNum != gSaveBlock1Ptr->location.mapNum)
        return script;
    if (scriptData->objectId != objectId)
        return script;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
    {
        ClearRamScript();
        return script;
    }
    else
    {
        gRamScriptRetAddr = script;
        return scriptData->script;
    }
}

bool32 ValidateRamScript(void)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return FALSE;
    if (scriptData->mapGroup != MAP_GROUP(MAP_UNDEFINED))
        return FALSE;
    if (scriptData->mapNum != MAP_NUM(MAP_UNDEFINED))
        return FALSE;
    if (scriptData->objectId != 0xFF)
        return FALSE;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
        return FALSE;
    return TRUE;
}

u8 *GetSavedRamScriptIfValid(void)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    if (!ValidateSavedWonderCard())
        return NULL;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return NULL;
    if (scriptData->mapGroup != MAP_GROUP(MAP_UNDEFINED))
        return NULL;
    if (scriptData->mapNum != MAP_NUM(MAP_UNDEFINED))
        return NULL;
    if (scriptData->objectId != 0xFF)
        return NULL;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
    {
        ClearRamScript();
        return NULL;
    }
    else
    {
        return scriptData->script;
    }
}

void InitRamScript_NoObjectEvent(u8 *script, u16 scriptSize)
{
    if (scriptSize > sizeof(gSaveBlock1Ptr->ramScript.data.script))
        scriptSize = sizeof(gSaveBlock1Ptr->ramScript.data.script);
    InitRamScript(script, scriptSize, MAP_GROUP(MAP_UNDEFINED), MAP_NUM(MAP_UNDEFINED), 0xFF);
}
