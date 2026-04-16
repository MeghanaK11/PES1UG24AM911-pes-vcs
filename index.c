// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            memmove(&index->entries[i], &index->entries[i + 1],
                    remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;

    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;

    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (strcmp(ent->d_name, ".pes") == 0)
                continue;
            if (strcmp(ent->d_name, "pes") == 0)
                continue;
            if (strstr(ent->d_name, ".o") != NULL)
                continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }

    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0;

    while (1) {
        IndexEntry entry;
        char hash_hex[HASH_HEX_SIZE + 1];

        int ret = fscanf(f, "%o %64s %ld %u %255s",
                         &entry.mode,
                         hash_hex,
                         &entry.mtime_sec,
                         &entry.size,
                         entry.path);

        if (ret != 5) break;

        hex_to_hash(hash_hex, &entry.hash);

        if (index->count < MAX_INDEX_ENTRIES) {
            index->entries[index->count++] = entry;
        }
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
int index_save(const Index *index) {
    mkdir(".pes", 0755);

    FILE *f = fopen(".pes/index", "w");
    if (!f) return -1;

    for (int i = 0; i < index->count; i++) {
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&index->entries[i].hash, hash_hex);

        fprintf(f, "%o %s %ld %u %s\n",
                index->entries[i].mode,
                hash_hex,
                index->entries[i].mtime_sec,
                index->entries[i].size,
                index->entries[i].path);
    }

    fclose(f);
    return 0;
}

// Stage a file for the next commit.
int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz < 0) {
        fclose(f);
        return -1;
    }

    size_t size = (size_t)sz;

    char *data = malloc(size > 0 ? size : 1);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (size > 0) fread(data, 1, size, f);
    fclose(f);

    ObjectID hash;
    if (object_write(OBJ_BLOB, data, size, &hash) != 0) {
        free(data);
        return -1;
    }

    free(data);

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    IndexEntry *entry = index_find(index, path);

    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    entry->mode = st.st_mode;
    entry->mtime_sec = st.st_mtime;
    entry->size = st.st_size;
    entry->hash = hash;

    strncpy(entry->path, path, sizeof(entry->path));
    entry->path[sizeof(entry->path) - 1] = '\0';

    return 0;   // ❗ DO NOT save here (pes.c handles it)
}
