#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: minidb <database-file>\n");
        return 1;
    }

    (void)argv;
    char line[65536];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (strlen(line) == 0) continue;

        fprintf(stderr, "Error: SQL engine not yet implemented\n");
    }

    return 0;
}
