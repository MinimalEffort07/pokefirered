/**
 * @file field_weather_util.c
 * @brief Weather State Management and Cycling System
 *
 * FILE OVERVIEW:
 * This file manages the overworld weather state — saving, loading, translating,
 * and cycling weather types. The GBA Pokemon games feature dynamic weather that
 * affects both visuals (rain particles, fog overlays, sandstorm effects) and
 * gameplay (some Pokemon appear only in rain, etc.).
 *
 * Weather can be:
 * - Fixed: Assigned by the map header (e.g., Cinnabar Island is always sunny)
 * - Cycled: Certain routes (119, 123 in Ruby/Sapphire) cycle through weather
 *   patterns based on in-game time, creating variety on repeat visits
 * - Script-set: Events can change weather (e.g., Groudon causes drought)
 *
 * The weather value stored in the save data is the "translated" weather — cycled
 * weather types are resolved to their current concrete type before saving.
 *
 * GAME STATISTICS:
 * The game tracks how many times the player encounters rain (GAME_STAT_GOT_RAINED_ON).
 * This is one of many fun statistics displayed on the Trainer Card.
 */
#include "global.h"
#include "field_weather.h"
#include "overworld.h"
#include "constants/weather.h"

static u8 TranslateWeatherNum(u8 weather);
static void UpdateRainCounter(u8 newWeather, u8 oldWeather);

/**
 * FUNCTION: SetSavedWeather
 *
 * PURPOSE: Sets the weather in the save data, translating cycled weather types
 * to their current concrete type and updating rain statistics.
 *
 * @param weather — weather constant (may be a cycle type like WEATHER_ROUTE119_CYCLE)
 */
void SetSavedWeather(u32 weather)
{
    u8 oldWeather = gSaveBlock1Ptr->weather;
    /* Translate cycle types to concrete weather, then save */
    gSaveBlock1Ptr->weather = TranslateWeatherNum(weather);
    /* Track rain encounters for the Trainer Card statistics */
    UpdateRainCounter(gSaveBlock1Ptr->weather, oldWeather);
}

/**
 * FUNCTION: GetSav1Weather
 *
 * PURPOSE: Returns the current weather type stored in save data.
 *
 * @return The current weather constant (always a concrete type, never a cycle type)
 */
u8 GetSav1Weather(void)
{
    return gSaveBlock1Ptr->weather;
}

/**
 * FUNCTION: SetSavedWeatherFromCurrMapHeader
 *
 * PURPOSE: Sets the saved weather from the current map's header data.
 * Called when entering a new map to apply that map's default weather.
 *
 * GAME LOGIC:
 * Each map has a weather field in its header. When the player enters the map,
 * this function reads that weather, translates it (resolving cycle types),
 * and saves it as the current weather.
 */
void SetSavedWeatherFromCurrMapHeader(void)
{
    u8 oldWeather = gSaveBlock1Ptr->weather;
    gSaveBlock1Ptr->weather = TranslateWeatherNum(gMapHeader.weather);
    UpdateRainCounter(gSaveBlock1Ptr->weather, oldWeather);
}

/**
 * FUNCTION: SetWeather
 *
 * PURPOSE: Sets the weather and begins transitioning visual effects to match.
 * SetNextWeather causes a gradual transition from the current visual weather
 * to the new one (e.g., rain particles gradually appear).
 *
 * @param weather — the new weather type to transition to
 */
void SetWeather(u32 weather)
{
    SetSavedWeather(weather);
    SetNextWeather(GetSav1Weather());
}

/**
 * FUNCTION: SetWeather_Unused
 *
 * PURPOSE: Sets weather and applies it immediately (no gradual transition).
 * SetCurrentAndNextWeather sets both the current and target weather at once,
 * so the visual change is instant rather than animated.
 *
 * @param weather — the new weather type to apply immediately
 */
void SetWeather_Unused(u32 weather)
{
    SetSavedWeather(weather);
    SetCurrentAndNextWeather(GetSav1Weather());
}

/**
 * FUNCTION: DoCurrentWeather
 *
 * PURPOSE: Starts a gradual transition to the weather stored in save data.
 * Called when the overworld needs to re-apply the current weather (e.g.,
 * after returning from a menu or battle that paused weather effects).
 */
void DoCurrentWeather(void)
{
    u8 weather = GetSav1Weather();
    SetNextWeather(weather);
}

/**
 * FUNCTION: ResumePausedWeather
 *
 * PURPOSE: Instantly resumes the saved weather without a transition animation.
 * Used when weather was paused (not cleared) and needs to snap back immediately.
 */
void ResumePausedWeather(void)
{
    u8 weather = GetSav1Weather();
    SetCurrentAndNextWeather(weather);
}

/*
 * Weather cycle tables for routes that have variable weather.
 * These are indexed by gSaveBlock1Ptr->weatherCycleStage (0-3),
 * which advances once per in-game day via UpdateWeatherPerDay().
 *
 * Route 119 (Ruby/Sapphire): Alternates between sunny, rain, thunderstorm, rain.
 *   This makes it a notoriously rainy route — 3 out of 4 days have rain.
 *
 * Route 123 (Ruby/Sapphire): Mostly sunny with occasional rain.
 *   Only 1 out of 4 days has rain.
 *
 * NOTE: These routes are from Ruby/Sapphire and may not be directly used
 * in FireRed, but the code is shared from the common engine.
 */
static const u8 sWeatherCycleRoute119[] = {
    WEATHER_SUNNY,
    WEATHER_RAIN,
    WEATHER_RAIN_THUNDERSTORM,
    WEATHER_RAIN,
};

static const u8 sWeatherCycleRoute123[] = {
    WEATHER_SUNNY,
    WEATHER_SUNNY,
    WEATHER_RAIN,
    WEATHER_SUNNY,
};

/**
 * FUNCTION: TranslateWeatherNum
 *
 * PURPOSE: Converts a weather constant from a map header into a concrete weather
 * type. Most weather types pass through unchanged, but cycle types (ROUTE119_CYCLE,
 * ROUTE123_CYCLE) are resolved to their current day's weather using the cycle tables.
 *
 * HOW IT WORKS:
 * Simple pass-through for all concrete weather types. For cycle types, looks up
 * the current weatherCycleStage (0-3) in the appropriate cycle table to determine
 * what weather should be active today.
 *
 * @param weather — raw weather constant (may be a cycle type)
 * @return The concrete weather type to actually display
 */
static u8 TranslateWeatherNum(u8 weather)
{
    switch (weather)
    {
    case WEATHER_NONE:               return WEATHER_NONE;
    case WEATHER_SUNNY_CLOUDS:       return WEATHER_SUNNY_CLOUDS;
    case WEATHER_SUNNY:              return WEATHER_SUNNY;
    case WEATHER_RAIN:               return WEATHER_RAIN;
    case WEATHER_SNOW:               return WEATHER_SNOW;
    case WEATHER_RAIN_THUNDERSTORM:  return WEATHER_RAIN_THUNDERSTORM;
    case WEATHER_FOG_HORIZONTAL:     return WEATHER_FOG_HORIZONTAL;
    case WEATHER_VOLCANIC_ASH:       return WEATHER_VOLCANIC_ASH;
    case WEATHER_SANDSTORM:          return WEATHER_SANDSTORM;
    case WEATHER_FOG_DIAGONAL:       return WEATHER_FOG_DIAGONAL;
    case WEATHER_UNDERWATER:         return WEATHER_UNDERWATER;
    case WEATHER_SHADE:              return WEATHER_SHADE;
    case WEATHER_DROUGHT:            return WEATHER_DROUGHT;
    case WEATHER_DOWNPOUR:           return WEATHER_DOWNPOUR;
    case WEATHER_UNDERWATER_BUBBLES: return WEATHER_UNDERWATER_BUBBLES;
    /* Cycle types: look up current stage in the rotation table */
    case WEATHER_ROUTE119_CYCLE:     return sWeatherCycleRoute119[gSaveBlock1Ptr->weatherCycleStage];
    case WEATHER_ROUTE123_CYCLE:     return sWeatherCycleRoute123[gSaveBlock1Ptr->weatherCycleStage];
    default:                         return WEATHER_NONE;
    }
}

/**
 * FUNCTION: UpdateWeatherPerDay
 *
 * PURPOSE: Advances the weather cycle stage by a given number of days.
 * Called by the real-time clock system when the game detects that time has passed.
 *
 * GAME LOGIC:
 * The cycle stage wraps around every 4 days (mod 4), so the weather pattern
 * repeats on a 4-day cycle. This creates variety for routes with cycling weather.
 *
 * @param increment — number of days that have passed since last update
 */
void UpdateWeatherPerDay(u16 increment)
{
    u16 weatherStage = gSaveBlock1Ptr->weatherCycleStage + increment;
    weatherStage %= 4;  /* Wrap to 0-3 range */
    gSaveBlock1Ptr->weatherCycleStage = weatherStage;
}

/**
 * FUNCTION: UpdateRainCounter
 *
 * PURPOSE: Increments the "got rained on" game statistic when the player
 * transitions into rainy or stormy weather from a different weather type.
 *
 * GAME LOGIC:
 * The game tracks many statistics for fun, including how many times the player
 * encountered rain. This is displayed on the Trainer Card's statistics page.
 * Only counts transitions (entering a new rainy area), not staying in rain.
 *
 * @param newWeather — the weather type being transitioned TO
 * @param oldWeather — the weather type being transitioned FROM
 */
static void UpdateRainCounter(u8 newWeather, u8 oldWeather)
{
    if (newWeather != oldWeather
        && (newWeather == WEATHER_RAIN || newWeather == WEATHER_RAIN_THUNDERSTORM))
        IncrementGameStat(GAME_STAT_GOT_RAINED_ON);
}
