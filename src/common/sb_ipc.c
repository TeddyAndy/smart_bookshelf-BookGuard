#include "sb_ipc.h"

#include <stdio.h>
#include <unistd.h>

int sb_write_text_atomic(const char *path, const char *text)
{
    char tmp[256];
    FILE *fp;

    if (!path || !path[0] || !text) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());

    fp = fopen(tmp, "w");
    if (!fp) return -1;
    if (fputs(text, fp) < 0) {
        fclose(fp);
        unlink(tmp);
        return -1;
    }
    fflush(fp);
    fsync(fileno(fp));
    if (fclose(fp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
