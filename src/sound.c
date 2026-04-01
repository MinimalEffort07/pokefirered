/**
 * sound.c - High-Level Sound System Interface
 *
 * ============================================================================
 * GBA SOUND SYSTEM OVERVIEW
 * ============================================================================
 *
 * The GBA has two sound subsystems:
 *
 * 1. HARDWARE CHANNELS (legacy Game Boy channels):
 *    - Channel 1: Square wave with frequency sweep
 *    - Channel 2: Square wave (no sweep)
 *    - Channel 3: Programmable waveform (4-bit samples)
 *    - Channel 4: Noise generator
 *    These are controlled via registers at 0x04000060 - 0x04000084.
 *
 * 2. DIRECT SOUND CHANNELS (DMA channels A and B):
 *    - Two 8-bit PCM (digitized audio) channels
 *    - Fed by DMA from a circular buffer in RAM
 *    - Can play digitized music, voice, and sound effects
 *
 * THE M4A SOUND ENGINE:
 * Pokemon games use a custom sound engine called "M4A" (MusicPlayer2000/MP2000),
 * originally written by Nintendo. M4A mixes multiple software channels and
 * outputs to the hardware Direct Sound channels. It supports:
 *   - MIDI-like music sequencing (notes, instruments, tempo, etc.)
 *   - Multiple simultaneous "music players" (MusicPlayerInfo structs)
 *   - Sound effects that can play alongside background music
 *   - Volume control, panning, pitch bending, and fade effects
 *
 * MUSIC PLAYERS IN THIS GAME:
 *   gMPlayInfo_BGM: Background music (only one BGM plays at a time)
 *   gMPlayInfo_SE1: Sound effect channel 1 (button presses, menu sounds)
 *   gMPlayInfo_SE2: Sound effect channel 2 (additional concurrent SFX)
 *   gMPlayInfo_SE3: Special sound effects (system sounds)
 *
 * SOUND MANAGEMENT ARCHITECTURE:
 * This file provides a high-level interface over M4A, managing:
 *   - Map music: What BGM plays on each map, with transitions between them
 *   - Fanfares: Short jingles (level up, item obtained) that pause the BGM
 *   - Pokemon cries: Digitized Pokemon sounds with pitch/volume/pan control
 *   - Sound effects: UI sounds and gameplay audio
 *   - BGM ducking: Temporarily reducing BGM volume during cries
 *
 * ============================================================================
 */

#include "global.h"
#include "gba/m4a_internal.h"
#include "gflib.h"
#include "battle.h"
#include "quest_log.h"
#include "m4a.h"
#include "constants/songs.h"
#include "constants/sound.h"
#include "task.h"

/**
 * Fanfare struct: Associates a song number with how many frames it plays.
 * Fanfares are short musical jingles that temporarily interrupt the BGM
 * (like the "level up" or "item obtained" sounds). The duration tells the
 * system how many frames to wait before resuming the BGM.
 */
struct Fanfare
{
    u16 songNum;   /* M4A song ID for this fanfare */
    u16 duration;  /* How many frames before the BGM resumes */
};

// TODO: what are these
extern u8 gDisableMapMusicChangeOnMapLoad;
extern u8 gDisableHelpSystemVolumeReduce;

// ewram
/*
 * gMPlay_PokemonCry: Pointer to the MusicPlayerInfo used for the currently
 * playing Pokemon cry. Pokemon cries use a separate player from BGM/SE.
 *
 * gPokemonCryBGMDuckingCounter: Frame counter for how long the BGM stays
 * ducked (reduced volume) after a cry starts. Gives the cry a head start
 * before checking if it has finished.
 */
EWRAM_DATA struct MusicPlayerInfo* gMPlay_PokemonCry = NULL;
EWRAM_DATA u8 gPokemonCryBGMDuckingCounter = 0;

// iwram bss
/*
 * Map music state machine variables:
 * sCurrentMapMusic:   Song ID of the music currently playing or about to play
 * sNextMapMusic:      Song ID queued to play after a fade-out completes
 * sMapMusicState:     Current state of the music transition state machine
 * sMapMusicFadeInSpeed: How fast the new song fades in (higher = faster)
 * sFanfareCounter:    Frames remaining before the current fanfare ends
 */
static u16 sCurrentMapMusic;
static u16 sNextMapMusic;
static u8 sMapMusicState;
static u8 sMapMusicFadeInSpeed;
static u16 sFanfareCounter;

/* Global flag to disable all background music (useful for debugging or special modes) */
COMMON_DATA bool8 gDisableMusic = 0;

/* External references to M4A sound engine structures and data */
extern u32 gBattleTypeFlags;
extern struct MusicPlayerInfo gMPlayInfo_BGM;   /* Background music player */
extern struct MusicPlayerInfo gMPlayInfo_SE1;   /* Sound effect player 1 */
extern struct MusicPlayerInfo gMPlayInfo_SE2;   /* Sound effect player 2 */
extern struct MusicPlayerInfo gMPlayInfo_SE3;   /* Special SE player (system sounds) */
extern struct ToneData gCryTable[];             /* Pokemon cry instrument data (forward) */
extern struct ToneData gCryTable_Reverse[];     /* Pokemon cry instrument data (reversed, for echo effects) */

extern u16 SpeciesToCryId(u16);

static void Task_Fanfare(u8 taskId);
static void CreateFanfareTask(void);
static void Task_DuckBGMForPokemonCry(u8 taskId);
static void RestoreBGMVolumeAfterPokemonCry(void);

/**
 * sFanfares: Lookup table of all fanfare jingles and their durations.
 *
 * Each entry maps a FANFARE_* constant to the song number and how many
 * frames the fanfare plays before the BGM resumes. Duration is measured
 * in frames (60 fps on GBA), so 160 frames = ~2.67 seconds.
 */
static const struct Fanfare sFanfares[] = {
    [FANFARE_LEVEL_UP]      = { MUS_LEVEL_UP,         80 },   /* ~1.3 seconds */
    [FANFARE_OBTAIN_ITEM]   = { MUS_OBTAIN_ITEM,     160 },   /* ~2.7 seconds */
    [FANFARE_EVOLVED]       = { MUS_EVOLVED,         220 },   /* ~3.7 seconds */
    [FANFARE_OBTAIN_TMHM]   = { MUS_OBTAIN_TMHM,     220 },
    [FANFARE_HEAL]          = { MUS_HEAL,            160 },
    [FANFARE_OBTAIN_BADGE]  = { MUS_OBTAIN_BADGE,    340 },   /* ~5.7 seconds (longest) */
    [FANFARE_MOVE_DELETED]  = { MUS_MOVE_DELETED,    180 },
    [FANFARE_OBTAIN_BERRY]  = { MUS_OBTAIN_BERRY,    120 },
    [FANFARE_SLOTS_JACKPOT] = { MUS_SLOTS_JACKPOT,   250 },
    [FANFARE_SLOTS_WIN]     = { MUS_SLOTS_WIN,       150 },
    [FANFARE_TOO_BAD]       = { MUS_TOO_BAD,         160 },
    [FANFARE_POKE_FLUTE]    = { MUS_POKE_FLUTE,      450 },   /* ~7.5 seconds */
    [FANFARE_KEY_ITEM]      = { MUS_OBTAIN_KEY_ITEM, 170 },
    [FANFARE_DEX_EVAL]      = { MUS_DEX_RATING,      196 }
};

/**
 * FUNCTION: InitMapMusic
 *
 * PURPOSE: Initialize the map music system at game startup.
 *
 * HOW IT WORKS:
 * Enables music playback (clears the disable flag) and resets the music
 * state machine to its initial state.
 */
void InitMapMusic(void)
{
    gDisableMusic = FALSE;
    ResetMapMusic();
}

/**
 * FUNCTION: MapMusicMain
 *
 * PURPOSE: Main update function for the map music state machine, called every frame.
 *
 * HOW IT WORKS:
 * Manages music transitions using a state machine with these states:
 *
 *   State 0: IDLE -- No music change pending. Do nothing.
 *
 *   State 1: PLAY_REQUESTED -- A new song was requested. Transitions to state 2
 *            and starts playing the song immediately.
 *
 *   State 2: PLAYING -- Music is currently playing normally. Do nothing.
 *
 *   State 3, 4: Reserved/unused transitional states.
 *
 *   State 5: FADING_OUT_TO_STOP -- Music is fading out to silence. Waits for
 *            the fade to complete (IsBGMStopped), then resets to idle (state 0).
 *
 *   State 6: FADING_OUT_TO_CHANGE -- Music is fading out before switching to a
 *            new song. Waits for both the fade-out AND any playing fanfare to
 *            finish, then starts the new song immediately.
 *
 *   State 7: FADING_OUT_TO_FADE_IN -- Like state 6 but the new song fades in
 *            gradually instead of starting at full volume. Creates a smooth
 *            crossfade effect between songs.
 *
 * GAME LOGIC:
 * This function runs in the main game loop. When the player moves between
 * areas with different music (e.g., entering a gym), the game requests a
 * music change, and this state machine handles the fade-out/fade-in transition
 * over multiple frames for a smooth audio experience.
 */
void MapMusicMain(void)
{
    switch (sMapMusicState)
    {
    case 0:
        /* IDLE: nothing to do */
        break;
    case 1:
        /* PLAY_REQUESTED: start the song and transition to PLAYING */
        sMapMusicState = 2;
        PlayBGM(sCurrentMapMusic);
        break;
    case 2:
    case 3:
    case 4:
        /* PLAYING / reserved: nothing to do */
        break;
    case 5:
        /* FADING_OUT_TO_STOP: wait for BGM to finish fading out */
        if (IsBGMStopped())
        {
            sNextMapMusic = 0;
            sMapMusicState = 0;  /* Return to IDLE */
        }
        break;
    case 6:
        /* FADING_OUT_TO_CHANGE: wait for fade-out and fanfare, then play new song */
        if (IsBGMStopped() && IsFanfareTaskInactive())
        {
            sCurrentMapMusic = sNextMapMusic;
            sNextMapMusic = 0;
            sMapMusicState = 2;  /* Transition to PLAYING */
            PlayBGM(sCurrentMapMusic);
        }
        break;
    case 7:
        /* FADING_OUT_TO_FADE_IN: wait, then fade in the new song gradually */
        if (IsBGMStopped() && IsFanfareTaskInactive())
        {
            FadeInNewBGM(sNextMapMusic, sMapMusicFadeInSpeed);
            sCurrentMapMusic = sNextMapMusic;
            sNextMapMusic = 0;
            sMapMusicState = 2;
            sMapMusicFadeInSpeed = 0;
        }
        break;
    }
}

/**
 * FUNCTION: ResetMapMusic
 *
 * PURPOSE: Reset the map music state machine to its initial state.
 *
 * HOW IT WORKS:
 * Clears all music tracking variables. Does NOT stop currently playing music --
 * it just resets the state machine so no transitions are pending.
 */
void ResetMapMusic(void)
{
    sCurrentMapMusic = 0;
    sNextMapMusic = 0;
    sMapMusicState = 0;
    sMapMusicFadeInSpeed = 0;
}

/**
 * FUNCTION: GetCurrentMapMusic
 *
 * PURPOSE: Return the song ID of the currently active map music.
 *
 * RETURNS: Song number, or 0 if no map music is set
 */
u16 GetCurrentMapMusic(void)
{
    return sCurrentMapMusic;
}

/**
 * FUNCTION: PlayNewMapMusic
 *
 * PURPOSE: Immediately start playing a new map song (no fade transition).
 *
 * HOW IT WORKS:
 * Sets the current map music and transitions the state machine to state 1
 * (PLAY_REQUESTED), which will start playing on the next MapMusicMain call.
 *
 * PARAMETERS:
 * @param songNum -- M4A song ID to play
 */
void PlayNewMapMusic(u16 songNum)
{
    sCurrentMapMusic = songNum;
    sNextMapMusic = 0;
    sMapMusicState = 1;
}

/**
 * FUNCTION: StopMapMusic
 *
 * PURPOSE: Stop map music by playing song 0 (silence).
 *
 * HOW IT WORKS:
 * Sets the current music to 0 and requests playback, which effectively
 * stops the BGM since song 0 is silence.
 */
void StopMapMusic(void)
{
    sCurrentMapMusic = 0;
    sNextMapMusic = 0;
    sMapMusicState = 1;
}

/**
 * FUNCTION: FadeOutMapMusic
 *
 * PURPOSE: Gradually fade out the current map music to silence.
 *
 * HOW IT WORKS:
 * Initiates a fade-out on the BGM player and sets state 5 (FADING_OUT_TO_STOP).
 * The state machine will wait until the fade completes before resetting to idle.
 *
 * PARAMETERS:
 * @param speed -- Fade speed (higher = faster fade). Each frame, volume decreases
 *                 by this amount. At 60fps, speed=4 takes about 1 second to silence.
 */
void FadeOutMapMusic(u8 speed)
{
    if (IsNotWaitingForBGMStop())
        FadeOutBGM(speed);
    sCurrentMapMusic = 0;
    sNextMapMusic = 0;
    sMapMusicState = 5;
}

/**
 * FUNCTION: FadeOutAndPlayNewMapMusic
 *
 * PURPOSE: Fade out current music, then start a new song at full volume.
 *
 * HOW IT WORKS:
 * Fades out the current BGM and queues the new song in sNextMapMusic.
 * State 6 (FADING_OUT_TO_CHANGE) will start the new song once the fade-out
 * and any playing fanfare complete.
 *
 * PARAMETERS:
 * @param songNum -- New song to play after fade-out
 * @param speed   -- Fade-out speed
 */
void FadeOutAndPlayNewMapMusic(u16 songNum, u8 speed)
{
    FadeOutMapMusic(speed);
    sCurrentMapMusic = 0;
    sNextMapMusic = songNum;
    sMapMusicState = 6;
}

/**
 * FUNCTION: FadeOutAndFadeInNewMapMusic
 *
 * PURPOSE: Fade out current music, then fade in a new song (smooth crossfade).
 *
 * HOW IT WORKS:
 * Combines fade-out and fade-in: the current song fades out at fadeOutSpeed,
 * then the new song fades in at fadeInSpeed. State 7 handles the transition.
 *
 * GAME LOGIC:
 * Used for smooth music transitions, like when entering or leaving buildings
 * where the indoor and outdoor themes should blend.
 *
 * PARAMETERS:
 * @param songNum      -- New song to fade in
 * @param fadeOutSpeed -- Speed for fading out the current song
 * @param fadeInSpeed  -- Speed for fading in the new song
 */
void FadeOutAndFadeInNewMapMusic(u16 songNum, u8 fadeOutSpeed, u8 fadeInSpeed)
{
    FadeOutMapMusic(fadeOutSpeed);
    sCurrentMapMusic = 0;
    sNextMapMusic = songNum;
    sMapMusicState = 7;
    sMapMusicFadeInSpeed = fadeInSpeed;
}

/**
 * FUNCTION: FadeInNewMapMusic (Unused)
 *
 * PURPOSE: Start a new song with a fade-in effect, without fading out first.
 *
 * PARAMETERS:
 * @param songNum -- Song to fade in
 * @param speed   -- Fade-in speed
 */
// Unused
static void FadeInNewMapMusic(u16 songNum, u8 speed)
{
    FadeInNewBGM(songNum, speed);
    sCurrentMapMusic = songNum;
    sNextMapMusic = 0;
    sMapMusicState = 2;
    sMapMusicFadeInSpeed = 0;
}

/**
 * FUNCTION: IsNotWaitingForBGMStop
 *
 * PURPOSE: Check whether the music system is in a state where new fade-outs are allowed.
 *
 * HOW IT WORKS:
 * Returns FALSE if the system is in states 5, 6, or 7 (already fading out
 * or waiting for a fade to complete). This prevents overlapping fade requests.
 *
 * RETURNS: TRUE if the system is idle/playing, FALSE if a transition is in progress
 */
bool8 IsNotWaitingForBGMStop(void)
{
    if (sMapMusicState == 6)
        return FALSE;
    if (sMapMusicState == 5)
        return FALSE;
    if (sMapMusicState == 7)
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: PlayFanfareByFanfareNum
 *
 * PURPOSE: Play a fanfare jingle by its index in the sFanfares table.
 *
 * HOW IT WORKS:
 * 1. If in Quest Log playback mode, sets the counter to 0xFF (max) to
 *    skip the actual sound but still track that a fanfare "happened"
 * 2. Otherwise, stops the BGM, starts the fanfare song, and sets
 *    the countdown timer to the fanfare's duration
 *
 * GAME LOGIC:
 * Fanfares temporarily replace the BGM. The BGM music player is stopped
 * (paused), the fanfare plays, and after sFanfareCounter frames the BGM
 * resumes from where it left off.
 *
 * PARAMETERS:
 * @param fanfareNum -- Index into the sFanfares[] array
 */
void PlayFanfareByFanfareNum(u8 fanfareNum)
{
    u16 songNum;
    if(gQuestLogState == QL_STATE_PLAYBACK)
    {
        /* During Quest Log playback, don't actually play audio */
        sFanfareCounter = 0xFF;
    }
    else
    {
        m4aMPlayStop(&gMPlayInfo_BGM);           /* Pause the BGM */
        songNum = sFanfares[fanfareNum].songNum;
        sFanfareCounter = sFanfares[fanfareNum].duration;
        m4aSongNumStart(songNum);                /* Start the fanfare */
    }
}

/**
 * FUNCTION: WaitFanfare
 *
 * PURPOSE: Wait for the current fanfare to finish, then resume or stop BGM.
 *
 * HOW IT WORKS:
 * Called each frame to count down the fanfare timer. While counting down,
 * returns FALSE (not done yet). When the counter reaches 0:
 *   - If stop is FALSE: resumes the paused BGM from where it left off
 *   - If stop is TRUE: starts MUS_DUMMY (silent song) instead of resuming
 *
 * PARAMETERS:
 * @param stop -- FALSE = resume BGM after fanfare, TRUE = stay silent
 *
 * RETURNS: FALSE while fanfare is playing, TRUE when finished
 */
bool8 WaitFanfare(bool8 stop)
{
    if (sFanfareCounter)
    {
        sFanfareCounter--;
        return FALSE;
    }
    else
    {
        if (!stop)
            m4aMPlayContinue(&gMPlayInfo_BGM);  /* Resume BGM from pause */
        else
            m4aSongNumStart(MUS_DUMMY);          /* Play silence instead */

        return TRUE;
    }
}

/**
 * FUNCTION: StopFanfareByFanfareNum (Unused)
 *
 * PURPOSE: Immediately stop a specific fanfare song.
 *
 * PARAMETERS:
 * @param fanfareNum -- Index into sFanfares[] to stop
 */
// Unused
void StopFanfareByFanfareNum(u8 fanfareNum)
{
    m4aSongNumStop(sFanfares[fanfareNum].songNum);
}

/**
 * FUNCTION: PlayFanfare
 *
 * PURPOSE: Play a fanfare by song number (looks up the duration automatically).
 *
 * HOW IT WORKS:
 * Searches the sFanfares table for a matching song number. If found, plays
 * that fanfare with its associated duration. If not found (the song isn't in
 * the table), falls back to playing the first fanfare (FANFARE_LEVEL_UP).
 *
 * Also creates a background task (Task_Fanfare) that monitors the countdown
 * and automatically resumes BGM when the fanfare finishes.
 *
 * PARAMETERS:
 * @param songNum -- M4A song ID to play as a fanfare
 */
void PlayFanfare(u16 songNum)
{
    s32 i;
    for (i = 0; (u32)i < ARRAY_COUNT(sFanfares); i++)
    {
        if (sFanfares[i].songNum == songNum)
        {
            PlayFanfareByFanfareNum(i);
            CreateFanfareTask();
            return;
        }
    }

    /* songNum is not in sFanfares -- play first fanfare as fallback */
    PlayFanfareByFanfareNum(0);
    CreateFanfareTask();
}

/**
 * FUNCTION: IsFanfareTaskInactive
 *
 * PURPOSE: Check whether any fanfare task is currently running.
 *
 * RETURNS: TRUE if no fanfare is playing, FALSE if one is active
 */
bool8 IsFanfareTaskInactive(void)
{
    if (FuncIsActiveTask(Task_Fanfare) == TRUE)
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: Task_Fanfare
 *
 * PURPOSE: Background task that counts down the fanfare timer and resumes BGM.
 *
 * HOW IT WORKS:
 * Runs every frame. Decrements sFanfareCounter each frame. When it reaches 0,
 * resumes the BGM player (which was paused when the fanfare started) and
 * destroys itself.
 *
 * This is the automatic version of WaitFanfare -- it runs in the background
 * so game code doesn't need to manually poll WaitFanfare each frame.
 *
 * PARAMETERS:
 * @param taskId -- ID of this task in the task system
 */
static void Task_Fanfare(u8 taskId)
{
    if (sFanfareCounter)
    {
        sFanfareCounter--;
    }
    else
    {
        m4aMPlayContinue(&gMPlayInfo_BGM);  /* Resume the paused BGM */
        DestroyTask(taskId);                /* Self-destruct */
    }
}

/**
 * FUNCTION: CreateFanfareTask
 *
 * PURPOSE: Create the background fanfare countdown task (if not already running).
 *
 * HOW IT WORKS:
 * Checks if Task_Fanfare is already active (to prevent duplicates), and if
 * not, creates it with priority 80 (medium priority in the task system).
 */
static void CreateFanfareTask(void)
{
    if (FuncIsActiveTask(Task_Fanfare) != TRUE)
        CreateTask(Task_Fanfare, 80);
}

/**
 * FUNCTION: FadeInNewBGM
 *
 * PURPOSE: Start a new background music song with a gradual fade-in effect.
 *
 * HOW IT WORKS:
 * 1. If music is globally disabled, plays song 0 (silence) instead
 * 2. Starts the song, immediately initializes (resets) the player
 * 3. Sets volume to 0 (silent) on all tracks
 * 4. Stops the song (so it's loaded but not audible)
 * 5. Calls m4aMPlayFadeIn to gradually increase volume over time
 *
 * This sequence ensures the song starts from the beginning at zero volume
 * and smoothly fades up to full volume.
 *
 * GBA CONTEXT:
 * m4aMPlayVolumeControl with value 0 = silence, 256 = full volume.
 * TRACKS_ALL applies the volume change to every track in the song.
 *
 * PARAMETERS:
 * @param songNum -- Song to fade in
 * @param speed   -- Fade-in speed (higher = faster volume increase per frame)
 */
void FadeInNewBGM(u16 songNum, u8 speed)
{
    if (gDisableMusic)
        songNum = 0;
    if (songNum == MUS_NONE)
        songNum = 0;
    m4aSongNumStart(songNum);                                  /* Load and start the song */
    m4aMPlayImmInit(&gMPlayInfo_BGM);                          /* Reset the player state */
    m4aMPlayVolumeControl(&gMPlayInfo_BGM, TRACKS_ALL, 0);     /* Set volume to 0 (silent) */
    m4aSongNumStop(songNum);                                   /* Pause the song at the start */
    m4aMPlayFadeIn(&gMPlayInfo_BGM, speed);                    /* Begin gradual fade-in */
}

/**
 * FUNCTION: FadeOutBGMTemporarily
 *
 * PURPOSE: Temporarily fade out the BGM (can be faded back in later).
 *
 * HOW IT WORKS:
 * Unlike FadeOutBGM which fades to silence and stops, this fades out but
 * keeps the player in a paused state so it can be resumed with FadeInBGM.
 *
 * PARAMETERS:
 * @param speed -- Fade-out speed
 */
void FadeOutBGMTemporarily(u8 speed)
{
    m4aMPlayFadeOutTemporarily(&gMPlayInfo_BGM, speed);
}

/**
 * FUNCTION: IsBGMPausedOrStopped
 *
 * PURPOSE: Check if the BGM is either paused or has no active tracks.
 *
 * HOW IT WORKS:
 * Checks the BGM player's status flags:
 *   MUSICPLAYER_STATUS_PAUSE: The player is paused (temporarily silent)
 *   MUSICPLAYER_STATUS_TRACK: At least one track is active (has notes to play)
 *
 * RETURNS: TRUE if BGM is paused or has no active tracks, FALSE if playing normally
 */
bool8 IsBGMPausedOrStopped(void)
{
    if (gMPlayInfo_BGM.status & MUSICPLAYER_STATUS_PAUSE)
        return TRUE;
    if (!(gMPlayInfo_BGM.status & MUSICPLAYER_STATUS_TRACK))
        return TRUE;
    return FALSE;
}

/**
 * FUNCTION: FadeInBGM
 *
 * PURPOSE: Fade in the BGM from its current (reduced) volume to full volume.
 *
 * PARAMETERS:
 * @param speed -- Fade-in speed (higher = faster)
 */
void FadeInBGM(u8 speed)
{
    m4aMPlayFadeIn(&gMPlayInfo_BGM, speed);
}

/**
 * FUNCTION: FadeOutBGM
 *
 * PURPOSE: Fade out the BGM to silence (permanently, until a new song starts).
 *
 * PARAMETERS:
 * @param speed -- Fade-out speed (higher = faster)
 */
void FadeOutBGM(u8 speed)
{
    m4aMPlayFadeOut(&gMPlayInfo_BGM, speed);
}

/**
 * FUNCTION: IsBGMStopped
 *
 * PURPOSE: Check if the BGM has completely stopped (no active tracks).
 *
 * RETURNS: TRUE if BGM is stopped, FALSE if any tracks are still active
 */
bool8 IsBGMStopped(void)
{
    if (!(gMPlayInfo_BGM.status & MUSICPLAYER_STATUS_TRACK))
        return TRUE;
    return FALSE;
}

/**
 * FUNCTION: PlayCry_Normal
 *
 * PURPOSE: Play a Pokemon's cry at normal speed/pitch with BGM volume ducking.
 *
 * HOW IT WORKS:
 * 1. Reduces BGM volume to 85/256 (~33%) so the cry is clearly audible
 * 2. Plays the cry using PlayCryInternal with default mode settings
 * 3. Starts a background task that will restore BGM volume once the cry finishes
 *
 * GAME LOGIC:
 * "Ducking" means reducing the volume of one audio source when another plays.
 * When you view a Pokemon in the Pokedex or it appears in battle, its cry
 * should be heard clearly over the music, so the BGM is temporarily quieted.
 *
 * PARAMETERS:
 * @param species -- Pokemon species number
 * @param pan     -- Stereo panning (-127 = full left, 0 = center, 127 = full right)
 */
void PlayCry_Normal(u16 species, s8 pan)
{
    m4aMPlayVolumeControl(&gMPlayInfo_BGM, TRACKS_ALL, 85);  /* Duck BGM to ~33% */
    PlayCryInternal(species, pan, CRY_VOLUME, CRY_PRIORITY_NORMAL, CRY_MODE_NORMAL);
    gPokemonCryBGMDuckingCounter = 2;  /* Wait 2 frames before checking if cry is done */
    RestoreBGMVolumeAfterPokemonCry();
}

/**
 * FUNCTION: PlayCry_NormalNoDucking
 *
 * PURPOSE: Play a Pokemon cry without reducing the BGM volume.
 *
 * HOW IT WORKS:
 * Directly plays the cry without any BGM volume manipulation. Used in
 * contexts where BGM ducking is handled externally or not desired.
 *
 * PARAMETERS:
 * @param species  -- Pokemon species number
 * @param pan      -- Stereo panning
 * @param volume   -- Cry volume
 * @param priority -- Sound priority (higher = takes precedence over other sounds)
 */
void PlayCry_NormalNoDucking(u16 species, s8 pan, s8 volume, u8 priority)
{
    PlayCryInternal(species, pan, volume, priority, CRY_MODE_NORMAL);
}

/**
 * FUNCTION: PlayCry_ByMode
 *
 * PURPOSE: Play a Pokemon cry with a specified mode (encounter, faint, etc.).
 *
 * HOW IT WORKS:
 * For CRY_MODE_DOUBLES, plays without ducking (both Pokemon cry at once in
 * double battles, so ducking would be excessive). For all other modes,
 * ducks the BGM and sets up volume restoration just like PlayCry_Normal.
 *
 * PARAMETERS:
 * @param species -- Pokemon species number
 * @param pan     -- Stereo panning
 * @param mode    -- Cry mode (normal, encounter, faint, echo, etc.)
 */
// Assuming it's not CRY_MODE_DOUBLES, this is equivalent to PlayCry_Normal except it allows other modes.
void PlayCry_ByMode(u16 species, s8 pan, u8 mode)
{
    if (mode == CRY_MODE_DOUBLES)
    {
        PlayCryInternal(species, pan, CRY_VOLUME, CRY_PRIORITY_NORMAL, mode);
    }
    else
    {
        m4aMPlayVolumeControl(&gMPlayInfo_BGM, TRACKS_ALL, 85);
        PlayCryInternal(species, pan, CRY_VOLUME, CRY_PRIORITY_NORMAL, mode);
        gPokemonCryBGMDuckingCounter = 2;
        RestoreBGMVolumeAfterPokemonCry();
    }
}

/**
 * FUNCTION: PlayCry_ReleaseDouble
 *
 * PURPOSE: Play a cry when releasing Pokemon in double battles.
 *
 * HOW IT WORKS:
 * In multi battles, the BGM is not ducked (since multiple cries may overlap).
 * In regular double battles, ducks the BGM for the first Pokemon only.
 *
 * PARAMETERS:
 * @param species -- Pokemon species number
 * @param pan     -- Stereo panning
 * @param mode    -- Cry mode
 */
// Used when releasing multiple Pokemon at once in battle.
void PlayCry_ReleaseDouble(u16 species, s8 pan, u8 mode)
{
    if (mode == CRY_MODE_DOUBLES)
    {
        PlayCryInternal(species, pan, CRY_VOLUME, CRY_PRIORITY_NORMAL, mode);
    }
    else
    {
        if (!(gBattleTypeFlags & BATTLE_TYPE_MULTI))
            m4aMPlayVolumeControl(&gMPlayInfo_BGM, TRACKS_ALL, 85);
        PlayCryInternal(species, pan, CRY_VOLUME, CRY_PRIORITY_NORMAL, mode);
    }
}

/**
 * FUNCTION: PlayCry_Script
 *
 * PURPOSE: Play a Pokemon cry triggered by a game script command.
 *
 * HOW IT WORKS:
 * Similar to PlayCry_Normal but has a special check for Quest Log playback --
 * during QL playback, the actual sound is skipped but the ducking counter
 * and restoration task are still set up. This is a FireRed/LeafGreen-specific
 * behavior since these games have a Quest Log feature that Ruby/Sapphire don't.
 *
 * PARAMETERS:
 * @param species -- Pokemon species number
 * @param mode    -- Cry mode
 */
void PlayCry_Script(u16 species, u8 mode)
{
    if (!QL_IS_PLAYBACK_STATE) // This check is exclusive to FR/LG
    {
        m4aMPlayVolumeControl(&gMPlayInfo_BGM, TRACKS_ALL, 85);
        PlayCryInternal(species, 0, CRY_VOLUME, CRY_PRIORITY_NORMAL, mode);
    }
    gPokemonCryBGMDuckingCounter = 2;
    RestoreBGMVolumeAfterPokemonCry();
}

/**
 * FUNCTION: PlayCryInternal
 *
 * PURPOSE: Core function that configures and plays a Pokemon cry sound.
 *
 * HOW IT WORKS:
 * Pokemon cries are instrument tones stored in tables (gCryTable/gCryTable_Reverse).
 * This function configures the cry's audio parameters based on the mode, then
 * looks up the correct instrument tone and plays it.
 *
 * CRY MODES AND THEIR AUDIO SIGNATURES:
 *   NORMAL:      Default cry, standard pitch and length
 *   DOUBLES:     Short cry (20 frames) with release, for double battle sends
 *   ENCOUNTER:   Wild encounter cry -- slightly higher pitch with chorus
 *   HIGH_PITCH:  Short excited cry (evolution/learning moves)
 *   ECHO_START:  First part of echo effect -- reversed, with heavy chorus
 *   FAINT:       Cry when Pokemon faints -- lower pitch (sad/deflated sound)
 *   ECHO_END:    Second part of echo effect -- normal direction, with chorus
 *   ROAR_1:      Short deep cry (start of Roar move)
 *   ROAR_2:      Long higher cry (end of Roar move)
 *   GROWL_1:     Short reversed growl (start of Growl move)
 *   GROWL_2:     Long growl (end of Growl move)
 *   WEAK:        Lower pitch cry (low HP or weakened state)
 *
 * The cry lookup uses a table system split into groups of 128:
 *   species 0-127   -> table 0 (gCryTable[0..127])
 *   species 128-255 -> table 1 (gCryTable[128..255])
 *   etc.
 *
 * Audio parameter meanings:
 *   pitch:   15360 = middle C (standard). Higher = higher pitch.
 *   length:  How many frames the main body of the cry plays
 *   release: How quickly the cry fades out (0 = instant, 225 = long tail)
 *   chorus:  Adds a detuned copy for a richer/wider sound (0 = none)
 *   reverse: If TRUE, plays the waveform backward (used for echo effects)
 *
 * PARAMETERS:
 * @param species  -- Pokemon species number (1-based, decremented internally)
 * @param pan      -- Stereo panning (-127 to +127)
 * @param volume   -- Cry volume
 * @param priority -- Sound priority
 * @param mode     -- Cry mode (CRY_MODE_NORMAL, CRY_MODE_ENCOUNTER, etc.)
 */
void PlayCryInternal(u16 species, s8 pan, s8 volume, u8 priority, u8 mode)
{
    bool32 reverse;
    u32 release;
    u32 length;
    u32 pitch;
    u32 chorus;
    u32 index;
    u8 table;

    species--;  /* Species are 1-based in the game but 0-based in the cry tables */

    /* Set default values -- may be overridden depending on mode */
    length = 140;
    reverse = FALSE;
    release = 0;
    pitch = 15360;   /* Standard pitch (middle C in M4A's pitch system) */
    chorus = 0;

    switch (mode)
    {
    case CRY_MODE_NORMAL:
        /* Use all defaults */
        break;
    case CRY_MODE_DOUBLES:
        /* Short cry for double battles -- quick with gradual release */
        length = 20;
        release = 225;
        break;
    case CRY_MODE_ENCOUNTER:
        /* Wild encounter cry -- slightly higher pitch, chorus for drama */
        release = 225;
        pitch = 15600;   /* Slightly above standard */
        chorus = 20;
        volume = 90;
        break;
    case CRY_MODE_HIGH_PITCH:
        /* Excited/celebratory cry -- short, high, with chorus */
        length = 50;
        release = 200;
        pitch = 15800;   /* Noticeably higher than standard */
        chorus = 20;
        volume = 90;
        break;
    case CRY_MODE_ECHO_START:
        /* First half of echo effect -- reversed waveform, heavy chorus */
        length = 25;
        reverse = TRUE;  /* Play the waveform backward */
        release = 100;
        pitch = 15600;
        chorus = 192;    /* Heavy chorus for spacey echo effect */
        volume = 90;
        break;
    case CRY_MODE_FAINT:
        /* Fainting cry -- lower pitch conveys sadness/defeat */
        release = 200;
        pitch = 14440;   /* Nearly a whole tone below standard */
        break;
    case CRY_MODE_ECHO_END:
        /* Second half of echo -- normal direction, chorus */
        release = 220;
        pitch = 15555;
        chorus = 192;
        volume = 90; // FR/LG changed this from 70 to 90
        break;
    case CRY_MODE_ROAR_1:
        /* Start of Roar move -- short, low burst */
        length = 10;
        release = 100;
        pitch = 14848;   /* Below standard pitch */
        break;
    case CRY_MODE_ROAR_2:
        /* End of Roar move -- long, slightly higher */
        length = 60;
        release = 225;
        pitch = 15616;   /* Above standard */
        break;
    case CRY_MODE_GROWL_1:
        /* Start of Growl move -- short reversed growl */
        length = 15;
        reverse = TRUE;
        release = 125;
        pitch = 15200;   /* Slightly below standard */
        break;
    case CRY_MODE_GROWL_2:
        /* End of Growl move -- long sustained growl */
        length = 100;
        release = 225;
        pitch = 15200;
        break;
    case CRY_MODE_WEAK_DOUBLES:
        /* Weak cry in double battles */
        length = 20;
        release = 225;
        // fallthrough
    case CRY_MODE_WEAK:
        /* Weak/low HP cry -- lower pitch */
        pitch = 15000;
        break;
    }

    /* Configure the M4A cry player with the computed parameters */
    SetPokemonCryVolume(volume);
    SetPokemonCryPanpot(pan);
    SetPokemonCryPitch(pitch);
    SetPokemonCryLength(length);
    SetPokemonCryProgress(0);      /* Start from the beginning */
    SetPokemonCryRelease(release);
    SetPokemonCryChorus(chorus);
    SetPokemonCryPriority(priority);

    /*
     * Look up the cry instrument tone from the cry tables.
     * The tables are organized in groups of 128, so we split the species
     * index into a table number (species / 128) and an index within
     * that table (species % 128).
     */
    species = SpeciesToCryId(species);
    index = species % 128;
    table = species / 128;

    #define GET_CRY(speciesIndex, tableId, reversed) \
        ((reversed) ? &gCryTable_Reverse[(128 * (tableId)) + (speciesIndex)] : &gCryTable[(128 * (tableId)) + (speciesIndex)])

    switch (table)
    {
    case 0:
        gMPlay_PokemonCry = SetPokemonCryTone(GET_CRY(index, 0, reverse));
        break;
    case 1:
        gMPlay_PokemonCry = SetPokemonCryTone(GET_CRY(index, 1, reverse));
        break;
    case 2:
        gMPlay_PokemonCry = SetPokemonCryTone(GET_CRY(index, 2, reverse));
        break;
    case 3:
        gMPlay_PokemonCry = SetPokemonCryTone(GET_CRY(index, 3, reverse));
        break;
    }

    #undef GET_CRY
}

/**
 * FUNCTION: IsCryFinished
 *
 * PURPOSE: Check if a Pokemon cry has finished playing (including BGM restoration).
 *
 * HOW IT WORKS:
 * Checks if the BGM ducking task is still active. If it is, the cry is still
 * playing (or the BGM hasn't been restored yet). When the task is done,
 * clears the cry song data and returns TRUE.
 *
 * RETURNS: TRUE if the cry and BGM restoration are both complete
 */
bool8 IsCryFinished(void)
{
    if (FuncIsActiveTask(Task_DuckBGMForPokemonCry) == TRUE)
    {
        return FALSE;
    }
    else
    {
        ClearPokemonCrySongs();
        return TRUE;
    }
}

/**
 * FUNCTION: StopCryAndClearCrySongs
 *
 * PURPOSE: Immediately stop the current cry and clean up cry data.
 */
void StopCryAndClearCrySongs(void)
{
    m4aMPlayStop(gMPlay_PokemonCry);
    ClearPokemonCrySongs();
}

/**
 * FUNCTION: StopCry
 *
 * PURPOSE: Immediately stop the current cry without clearing cry data.
 */
void StopCry(void)
{
    m4aMPlayStop(gMPlay_PokemonCry);
}

/**
 * FUNCTION: IsCryPlayingOrClearCrySongs
 *
 * PURPOSE: Check if a cry is playing; if not, clear cry data.
 *
 * RETURNS: TRUE if still playing, FALSE if finished (and cry data was cleared)
 */
bool8 IsCryPlayingOrClearCrySongs(void)
{
    if (IsPokemonCryPlaying(gMPlay_PokemonCry))
    {
        return TRUE;
    }
    else
    {
        ClearPokemonCrySongs();
        return FALSE;
    }
}

/**
 * FUNCTION: IsCryPlaying
 *
 * PURPOSE: Simple check whether a Pokemon cry is currently playing.
 *
 * RETURNS: TRUE if playing, FALSE if not
 */
bool8 IsCryPlaying(void)
{
    if (IsPokemonCryPlaying(gMPlay_PokemonCry))
        return TRUE;
    else
        return FALSE;
}

/**
 * FUNCTION: Task_DuckBGMForPokemonCry
 *
 * PURPOSE: Background task that waits for a Pokemon cry to finish, then
 *          restores BGM volume to full.
 *
 * HOW IT WORKS:
 * First waits gPokemonCryBGMDuckingCounter frames (gives the cry a head
 * start before checking). Then polls each frame to see if the cry has
 * finished playing. Once it has, restores BGM volume to 256 (full) and
 * self-destructs.
 *
 * GBA CONTEXT:
 * Volume 256 = full volume (1.0x multiplier). The BGM was reduced to 85
 * (~33%) when the cry started. This task smoothly restores it.
 *
 * PARAMETERS:
 * @param taskId -- ID of this task
 */
static void Task_DuckBGMForPokemonCry(u8 taskId)
{
    if (gPokemonCryBGMDuckingCounter)
    {
        gPokemonCryBGMDuckingCounter--;
        return;
    }

    if (!IsPokemonCryPlaying(gMPlay_PokemonCry))
    {
        m4aMPlayVolumeControl(&gMPlayInfo_BGM, TRACKS_ALL, 256);  /* Restore to full volume */
        DestroyTask(taskId);
    }
}

/**
 * FUNCTION: RestoreBGMVolumeAfterPokemonCry
 *
 * PURPOSE: Create the BGM volume restoration task (if not already running).
 *
 * HOW IT WORKS:
 * Checks for an existing Task_DuckBGMForPokemonCry task and only creates
 * one if none exists. This prevents duplicate tasks from stacking up if
 * multiple cries are triggered in quick succession.
 */
static void RestoreBGMVolumeAfterPokemonCry(void)
{
    if (FuncIsActiveTask(Task_DuckBGMForPokemonCry) != TRUE)
        CreateTask(Task_DuckBGMForPokemonCry, 80);
}

/**
 * FUNCTION: PlayBGM
 *
 * PURPOSE: Play a background music song, respecting the global disable flag.
 *
 * HOW IT WORKS:
 * If music is globally disabled (gDisableMusic) or the song is MUS_NONE,
 * plays song 0 (silence). Otherwise starts the requested song.
 *
 * PARAMETERS:
 * @param songNum -- M4A song ID to play
 */
void PlayBGM(u16 songNum)
{
    if (gDisableMusic)
        songNum = 0;
    if (songNum == MUS_NONE)
        songNum = 0;
    m4aSongNumStart(songNum);
}

/**
 * FUNCTION: PlaySE
 *
 * PURPOSE: Play a sound effect (button press, menu sound, etc.).
 *
 * HOW IT WORKS:
 * Plays the sound effect unless map music changes are disabled on map load
 * or the game is in Quest Log playback mode (no audio during replay).
 *
 * PARAMETERS:
 * @param songNum -- M4A song ID for the sound effect
 */
void PlaySE(u16 songNum)
{
    if (gDisableMapMusicChangeOnMapLoad == 0 && gQuestLogState != QL_STATE_PLAYBACK)
        m4aSongNumStart(songNum);
}

/**
 * FUNCTION: PlaySE12WithPanning
 *
 * PURPOSE: Play a sound effect on both SE channels with stereo panning.
 *
 * HOW IT WORKS:
 * Starts the song, resets both SE players, and sets the same pan value on both.
 * Using both SE channels gives the sound more presence.
 *
 * GBA CONTEXT:
 * Pan values: -127 = full left, 0 = center, +127 = full right.
 * m4aMPlayImmInit resets the player's state to ensure clean playback.
 *
 * PARAMETERS:
 * @param songNum -- Sound effect song ID
 * @param pan     -- Stereo panning value
 */
void PlaySE12WithPanning(u16 songNum, s8 pan)
{
    m4aSongNumStart(songNum);
    m4aMPlayImmInit(&gMPlayInfo_SE1);
    m4aMPlayImmInit(&gMPlayInfo_SE2);
    m4aMPlayPanpotControl(&gMPlayInfo_SE1, TRACKS_ALL, pan);
    m4aMPlayPanpotControl(&gMPlayInfo_SE2, TRACKS_ALL, pan);
}

/**
 * FUNCTION: PlaySE1WithPanning
 *
 * PURPOSE: Play a sound effect on SE channel 1 with stereo panning.
 *
 * PARAMETERS:
 * @param songNum -- Sound effect song ID
 * @param pan     -- Stereo panning value
 */
void PlaySE1WithPanning(u16 songNum, s8 pan)
{
    m4aSongNumStart(songNum);
    m4aMPlayImmInit(&gMPlayInfo_SE1);
    m4aMPlayPanpotControl(&gMPlayInfo_SE1, TRACKS_ALL, pan);
}

/**
 * FUNCTION: PlaySE2WithPanning
 *
 * PURPOSE: Play a sound effect on SE channel 2 with stereo panning.
 *
 * PARAMETERS:
 * @param songNum -- Sound effect song ID
 * @param pan     -- Stereo panning value
 */
void PlaySE2WithPanning(u16 songNum, s8 pan)
{
    m4aSongNumStart(songNum);
    m4aMPlayImmInit(&gMPlayInfo_SE2);
    m4aMPlayPanpotControl(&gMPlayInfo_SE2, TRACKS_ALL, pan);
}

/**
 * FUNCTION: SE12PanpotControl
 *
 * PURPOSE: Update the stereo panning on both sound effect channels.
 *
 * HOW IT WORKS:
 * Changes the pan of already-playing sound effects. Used when a sound source
 * moves on screen (e.g., a Pokemon moving left or right during battle).
 *
 * PARAMETERS:
 * @param pan -- New stereo panning value (-127 to +127)
 */
void SE12PanpotControl(s8 pan)
{
    m4aMPlayPanpotControl(&gMPlayInfo_SE1, TRACKS_ALL, pan);
    m4aMPlayPanpotControl(&gMPlayInfo_SE2, TRACKS_ALL, pan);
}

/**
 * FUNCTION: IsSEPlaying
 *
 * PURPOSE: Check if any sound effect is currently playing on SE channels 1 or 2.
 *
 * HOW IT WORKS:
 * Returns FALSE if both SE channels are paused or have no active tracks.
 * Returns TRUE if either channel has active, unpaused tracks.
 *
 * RETURNS: TRUE if a sound effect is playing, FALSE otherwise
 */
bool8 IsSEPlaying(void)
{
    if ((gMPlayInfo_SE1.status & MUSICPLAYER_STATUS_PAUSE) && (gMPlayInfo_SE2.status & MUSICPLAYER_STATUS_PAUSE))
        return FALSE;
    if (!(gMPlayInfo_SE1.status & MUSICPLAYER_STATUS_TRACK) && !(gMPlayInfo_SE2.status & MUSICPLAYER_STATUS_TRACK))
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: IsBGMPlaying
 *
 * PURPOSE: Check if background music is actively playing (not paused, not stopped).
 *
 * RETURNS: TRUE if BGM is playing, FALSE if paused or stopped
 */
bool8 IsBGMPlaying(void)
{
    if (gMPlayInfo_BGM.status & MUSICPLAYER_STATUS_PAUSE)
        return FALSE;
    if (!(gMPlayInfo_BGM.status & MUSICPLAYER_STATUS_TRACK))
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: IsSpecialSEPlaying
 *
 * PURPOSE: Check if a sound effect is playing on the special SE channel (SE3).
 *
 * HOW IT WORKS:
 * SE3 is used for system-level sounds separate from gameplay SE.
 *
 * RETURNS: TRUE if SE3 is playing, FALSE otherwise
 */
bool8 IsSpecialSEPlaying(void)
{
    if (gMPlayInfo_SE3.status & MUSICPLAYER_STATUS_PAUSE)
        return FALSE;
    if (!(gMPlayInfo_SE3.status & MUSICPLAYER_STATUS_TRACK))
        return FALSE;
    return TRUE;
}

/**
 * FUNCTION: SetBGMVolume_SuppressHelpSystemReduction
 *
 * PURPOSE: Set BGM volume and prevent the Help System from reducing it further.
 *
 * HOW IT WORKS:
 * The Help System (a FireRed/LeafGreen feature) normally reduces BGM volume
 * when active. This function sets a flag to suppress that reduction, then
 * sets the BGM volume to the requested level.
 *
 * GAME LOGIC:
 * Used when specific game events need full volume control without the Help
 * System interfering (e.g., during cutscenes or special events).
 *
 * PARAMETERS:
 * @param volume -- Desired volume (0-256, where 256 = full)
 */
void SetBGMVolume_SuppressHelpSystemReduction(u16 volume)
{
    gDisableHelpSystemVolumeReduce = TRUE;
    m4aMPlayVolumeControl(&gMPlayInfo_BGM, TRACKS_ALL, volume);
}

/**
 * FUNCTION: BGMVolumeMax_EnableHelpSystemReduction
 *
 * PURPOSE: Restore BGM to full volume and re-enable Help System volume control.
 *
 * HOW IT WORKS:
 * Clears the suppression flag and sets BGM volume to 256 (full).
 * After this call, the Help System can once again reduce BGM volume normally.
 */
void BGMVolumeMax_EnableHelpSystemReduction(void)
{
    gDisableHelpSystemVolumeReduce = FALSE;
    m4aMPlayVolumeControl(&gMPlayInfo_BGM, TRACKS_ALL, 256);
}
