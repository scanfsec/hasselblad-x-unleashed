/* A successful write to an unlinked inode is not a saved recording. */
#ifndef MIC_RECORD_FILE_H
#define MIC_RECORD_FILE_H
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static int mic_record_file_present(FILE *file, const char *path)
{
    struct stat opened, named;
    return file && path && !fstat(fileno(file), &opened) &&
        !stat(path, &named) && S_ISREG(opened.st_mode) && opened.st_nlink > 0 &&
        opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
}
#endif
