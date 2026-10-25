/* Inspect the real SDL controller mapping/state without sending input to the game. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    for (int i=0; i<5; ++i) { SDL_PumpEvents(); SDL_Delay(40); }
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    if (!ids || !count) { fputs("No controller detected\n",stderr); return 2; }
    SDL_Gamepad *pad=SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    if (!pad) { fprintf(stderr, "%s\n", SDL_GetError()); return 3; }
    SDL_Joystick *joy=SDL_GetGamepadJoystick(pad);
    char *mapping=SDL_GetGamepadMapping(pad);
    printf("Controller: %s\nMapping: %s\nRaw axes=%d buttons=%d hats=%d\n",
           SDL_GetGamepadName(pad), mapping ? mapping : "none",
           SDL_GetNumJoystickAxes(joy), SDL_GetNumJoystickButtons(joy), SDL_GetNumJoystickHats(joy));
    SDL_free(mapping);
    const uint64_t end=SDL_GetTicks()+(argc>1 ? (uint64_t)atoi(argv[1])*1000 : 0);
    char previous[1024]="";
    do {
        SDL_PumpEvents(); SDL_UpdateGamepads();
        char state[1024];
        int used=snprintf(state,sizeof state,"raw buttons=");
        for (int i=0; i<SDL_GetNumJoystickButtons(joy) && i<64; ++i)
            if (SDL_GetJoystickButton(joy,i)) used+=snprintf(state+used,sizeof state-(size_t)used,"%d,",i);
        used+=snprintf(state+used,sizeof state-(size_t)used," axes=");
        for (int i=0; i<SDL_GetNumJoystickAxes(joy) && i<8; ++i)
            used+=snprintf(state+used,sizeof state-(size_t)used,"%d:%d,",i,SDL_GetJoystickAxis(joy,i));
        used+=snprintf(state+used,sizeof state-(size_t)used," hats=");
        for (int i=0; i<SDL_GetNumJoystickHats(joy) && i<4; ++i)
            used+=snprintf(state+used,sizeof state-(size_t)used,"%u,",SDL_GetJoystickHat(joy,i));
        snprintf(state+used,sizeof state-(size_t)used," SDL A=%d B=%d X=%d Y=%d L2=%d R2=%d dpad=%d%d%d%d",
                 SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_SOUTH),
                 SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_EAST),
                 SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_WEST),
                 SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_NORTH),
                 SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                 SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
                 SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_DPAD_UP),
                 SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_DPAD_DOWN),
                 SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_DPAD_LEFT),
                 SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
        if (strcmp(state,previous)) { printf("%llu %s\n",(unsigned long long)SDL_GetTicks(),state); strcpy(previous,state); }
        SDL_Delay(16);
    } while (SDL_GetTicks()<end);
    SDL_CloseGamepad(pad); SDL_Quit();
    return 0;
}
