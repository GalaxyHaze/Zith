#include <SDL2/SDL.h>

#include <stdbool.h>

bool zsdl_init(unsigned int flags) {
    return SDL_Init(flags) == 0;
}

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;

static bool ensure_window(void) {
    if (s_window != NULL && s_renderer != NULL)
        return true;

    if (SDL_CreateWindowAndRenderer(
            "Zith + SDL2",
            800,
            600,
            SDL_WINDOW_RESIZABLE,
            &s_window,
            &s_renderer) != 0)
        return false;

    return true;
}

bool zsdl_poll_quit(void) {
    SDL_Event event;

    while (SDL_PollEvent(&event))
        if (event.type == SDL_QUIT)
            return true;

    return false;
}

void zsdl_render_clear(void) {
    if (!ensure_window())
        return;

    SDL_SetRenderDrawColor(s_renderer, 30, 30, 46, 255);
    SDL_RenderClear(s_renderer);
}

void zsdl_render_rect(int x, int y, int w, int h) {
    if (!ensure_window())
        return;

    SDL_Rect rect = {x, y, w, h};
    SDL_SetRenderDrawColor(s_renderer, 61, 174, 233, 255);
    SDL_RenderFillRect(s_renderer, &rect);
}

void zsdl_render_present(void) {
    if (!ensure_window())
        return;
    SDL_RenderPresent(s_renderer);
}

void zsdl_delay(unsigned int ms) {
    SDL_Delay(ms);
}
