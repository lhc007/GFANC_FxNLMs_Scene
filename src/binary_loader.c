#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "binary_loader.h"

int bin_load_float(const char *path, float **data_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    int n = (int)(size / sizeof(float));
    float *d = (float *)malloc(size);
    if (!d) { fclose(f); return -1; }
    if (fread(d, 1, size, f) != (size_t)size) {
        free(d); fclose(f); return -1;
    }
    fclose(f);
    *data_out = d;
    return n;
}

void bin_free(float *data) { if (data) free(data); }

float *bin_load(const char *name, int *n_out)
{
    char path[512];
    snprintf(path, sizeof(path), "data/%s.bin", name);
    float *d = NULL;
    int n = bin_load_float(path, &d);
    if (n < 0) { *n_out = 0; return NULL; }
    *n_out = n;
    return d;
}
