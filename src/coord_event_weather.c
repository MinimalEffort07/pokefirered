/**
 * @file coord_event_weather.c
 * @brief Coordinate-Triggered Weather Change Events (Dummied Out)
 *
 * FILE OVERVIEW:
 * In Ruby/Sapphire/Emerald, certain map tiles had "coord events" — invisible
 * triggers that fired when the player stepped on them. Some of these triggered
 * weather changes (e.g., walking into a foggy area within a route). Each weather
 * type had a callback function that would be called when the player stepped on
 * the corresponding coord event tile.
 *
 * In FireRed/LeafGreen, ALL of these weather coord event callbacks have been
 * dummied out (empty function bodies) because Kanto doesn't use tile-based
 * weather transitions. Weather in FRLG is set per-map rather than per-tile.
 *
 * The dispatch function DoCoordEventWeather still exists and works — it just
 * calls empty functions that do nothing.
 */
#include "global.h"
#include "constants/weather.h"

/* All weather coord event callbacks are empty in FireRed/LeafGreen */
void WeatherCoordEvent_SunnyClouds(void) {}
void WeatherCoordEvent_Sunny(void) {}
void WeatherCoordEvent_Rain(void) {}
void WeatherCoordEvent_Snow(void) {}
void WeatherCoordEvent_RainThunderstorm(void) {}
void WeatherCoordEvent_FogHorizontal(void) {}
void WeatherCoordEvent_VolcanicAsh(void) {}
void WeatherCoordEvent_Sandstorm(void) {}
void WeatherCoordEvent_FogDiagonal(void) {}
void WeatherCoordEvent_Underwater(void) {}
void WeatherCoordEvent_Shade(void) {}
void WeatherCoordEvent_Route119Cycle(void) {}
void WeatherCoordEvent_Route123Cycle(void) {}

/*
 * Lookup table mapping weather IDs to their coord event callbacks.
 * Used by DoCoordEventWeather to find and call the right function.
 */
static struct {
    u8 weatherId;
    void (*callback)(void);
} const sWeatherCoordEventFuncs[] = {
    {WEATHER_SUNNY_CLOUDS,      WeatherCoordEvent_SunnyClouds     },
    {WEATHER_SUNNY,             WeatherCoordEvent_Sunny           },
    {WEATHER_RAIN,              WeatherCoordEvent_Rain            },
    {WEATHER_SNOW,              WeatherCoordEvent_Snow            },
    {WEATHER_RAIN_THUNDERSTORM, WeatherCoordEvent_RainThunderstorm},
    {WEATHER_FOG_HORIZONTAL,    WeatherCoordEvent_FogHorizontal   },
    {WEATHER_VOLCANIC_ASH,      WeatherCoordEvent_VolcanicAsh     },
    {WEATHER_SANDSTORM,         WeatherCoordEvent_Sandstorm       },
    {WEATHER_FOG_DIAGONAL,      WeatherCoordEvent_FogDiagonal     },
    {WEATHER_UNDERWATER,        WeatherCoordEvent_Underwater      },
    {WEATHER_SHADE,             WeatherCoordEvent_Shade           },
    {WEATHER_ROUTE119_CYCLE,    WeatherCoordEvent_Route119Cycle   },
    {WEATHER_ROUTE123_CYCLE,    WeatherCoordEvent_Route123Cycle   }
};

/**
 * FUNCTION: DoCoordEventWeather
 *
 * PURPOSE: Dispatches a weather coord event by looking up the weather ID in
 * the callback table and calling the corresponding function.
 *
 * GAME LOGIC:
 * In FireRed, all callbacks are empty, so this function effectively does nothing.
 * It's kept for code compatibility with the shared Ruby/Sapphire engine.
 *
 * @param weatherId — the weather type to trigger (WEATHER_* constant)
 */
void DoCoordEventWeather(u8 weatherId)
{
    u8 i;
    for (i = 0; i < NELEMS(sWeatherCoordEventFuncs); i++)
    {
        if (sWeatherCoordEventFuncs[i].weatherId == weatherId)
        {
            sWeatherCoordEventFuncs[i].callback();
            return;
        }
    }
}
