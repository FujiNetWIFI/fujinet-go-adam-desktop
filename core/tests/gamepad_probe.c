/*
 * Hardware probe (not a ctest): lists what SDL3 sees on the joystick and
 * gamepad APIs and streams state changes for a few seconds. Used to verify
 * real controllers against the session's gamepad backend, especially
 * generic pads with no gamecontrollerdb mapping.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 6;
    int i, n = 0;
    SDL_JoystickID *ids;
    Uint64 end;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_UpdateJoysticks();
    ids = SDL_GetJoysticks(&n);
    printf("joysticks: %d\n", n);
    for (i = 0; i < n; i++) {
        SDL_JoystickID id = ids[i];
        printf("  id=%u name=\"%s\" is_gamepad=%d\n", (unsigned)id,
               SDL_GetJoystickNameForID(id),
               SDL_IsGamepad(id) ? 1 : 0);
        if (SDL_IsGamepad(id)) {
            SDL_Gamepad *g = SDL_OpenGamepad(id);
            if (g)
                printf("    gamepad mapping: %s\n",
                       SDL_GetGamepadMapping(g));
        } else {
            SDL_Joystick *j = SDL_OpenJoystick(id);
            if (j)
                printf("    axes=%d buttons=%d hats=%d guid-mapped=no\n",
                       SDL_GetNumJoystickAxes(j),
                       SDL_GetNumJoystickButtons(j),
                       SDL_GetNumJoystickHats(j));
        }
    }
    SDL_free(ids);

    printf("polling %ds; press things...\n", seconds);
    end = SDL_GetTicks() + (Uint64)seconds * 1000;
    {
        char last[128] = "";
        while (SDL_GetTicks() < end) {
            char cur[128] = "";
            SDL_UpdateJoysticks();
            SDL_UpdateGamepads();
            ids = SDL_GetJoysticks(&n);
            for (i = 0; i < n && i < 1; i++) {
                SDL_Joystick *j = SDL_GetJoystickFromID(ids[i]);
                if (!j) j = SDL_OpenJoystick(ids[i]);
                if (j) {
                    int b, nb = SDL_GetNumJoystickButtons(j);
                    int pos = snprintf(cur, sizeof(cur), "x=%6d y=%6d b=",
                                       SDL_GetJoystickAxis(j, 0),
                                       SDL_GetJoystickAxis(j, 1));
                    for (b = 0; b < nb && pos < (int)sizeof(cur) - 2; b++)
                        cur[pos++] = SDL_GetJoystickButton(j, b) ? '1' : '0';
                    cur[pos] = '\0';
                }
            }
            SDL_free(ids);
            if (strcmp(cur, last) != 0) {
                printf("  %s\n", cur);
                fflush(stdout);
                strcpy(last, cur);
            }
            SDL_Delay(16);
        }
    }
    SDL_Quit();
    return 0;
}
