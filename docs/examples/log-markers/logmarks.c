// A Rekodr plugin's program: reads the lines a game adds to its log and prints a marker line for each event it
// knows. Rekodr starts it while mygame.exe is played and ends it afterwards. Build: cl /O2 logmarks.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\MyGame\\game.log", getenv("LOCALAPPDATA"));
    FILE *f = 0;
    while (!(f = fopen(path, "rb"))) Sleep(1000);
    fseek(f, 0, SEEK_END); // only what is written from now on
    char line[1024];
    for (;;) {
        if (!fgets(line, sizeof line, f)) {
            clearerr(f);
            Sleep(250);
            continue;
        }
        if (strstr(line, "You were eliminated")) puts("e death");
        else if (strstr(line, "You eliminated")) puts("e kill");
        else if (strstr(line, "VICTORY")) puts("e win Victory");
        fflush(stdout); // Rekodr stamps each line when it arrives
    }
}
