#include "index.h"
#include "pes.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// comparator for sorting
static int index_entry_cmp(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

// load index
int index_load(Index *index) {
    FILE *fp = fopen(".pes/index", "r");

    index->count = 0;

    if (!fp) return 0;

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        char hash_hex[65];

        if (fscanf(fp, "%o %64s %lu %u %[^\n]\n",
                   &e->mode,
                   hash_hex,
                   &e->mtime_sec,
                   &e->size,
                   e->path) != 5) {
            break;
        }

        hex_to_hash(hash_hex, &e->hash);
        index->count++;
    }

    fclose(fp);
    return 0;
}

// save index
int index_save(const Index *index) {
    FILE *fp = fopen(".pes/index.tmp", "w");
    if (!fp) return -1;

    Index sorted = *index;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), index_entry_cmp);

    for (int i = 0; i < sorted.count; i++) {
        char hex[65];
        hash_to_hex(&sorted.entries[i].hash, hex);

        fprintf(fp, "%o %s %lu %u %s\n",
                sorted.entries[i].mode,
                hex,
                sorted.entries[i].mtime_sec,
                sorted.entries[i].size,
                sorted.entries[i].path);
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    rename(".pes/index.tmp", ".pes/index");
    return 0;
}

// add file to index
int index_add(Index *index, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    if (size < 0) {
        fclose(fp);
        return -1;
    }

    void *data = malloc(size > 0 ? size : 1);
    if (!data) {
        fclose(fp);
        return -1;
    }

    if (size > 0 && fread(data, 1, size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return -1;
    }

    fclose(fp);

    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data);
        return -1;
    }

    free(data);

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    IndexEntry *e = index_find(index, path);

    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
    }

    e->mode = get_file_mode(path);
    e->hash = id;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';

    return 0;
}
