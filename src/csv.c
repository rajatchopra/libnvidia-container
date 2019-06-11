/*
 * Copyright (c) 2017-2019, NVIDIA CORPORATION. All rights reserved.
 */

#include <sys/mman.h>

#include <limits.h>
#include <stdalign.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "csv.h"
#include "utils.h"
#include "xfuncs.h"
#include "jetson_info.h"

#define MAX_NUM_FIELDS_PER_LINE 3

#if !(defined(TESTING) && TESTING == TRUE)
# define printf(...)
#endif

static void csv_pack(struct csv *);
static void trim(char **);

void
csv_init(struct csv *ctx, struct error *err, const char *path)
{
        *ctx = (struct csv){err, path, NULL, 0, NULL, 0};
}

int
csv_open(struct csv *ctx)
{
        ctx->base = file_map_prot(ctx->err, ctx->path, &ctx->size, PROT_READ | PROT_WRITE);
        if (ctx->base == NULL)
                return (-1);

        ctx->base = mremap(ctx->base, ctx->size, ctx->size + 1, MREMAP_MAYMOVE);
        if (ctx->base == NULL)
                return (-1);

        ((char *) ctx->base)[ctx->size] = '\0';
        ++ctx->size;

        return (0);
}


int
csv_close(struct csv *ctx)
{
        if (file_unmap(ctx->err, ctx->path, ctx->base, ctx->size) < 0)
                return (-1);

        ctx->base = NULL;
        ctx->size = 0;
        return (0);
}

void
csv_pack(struct csv *ctx)
{
        size_t idx = 0;

        if (ctx->lines == NULL)
                return;

        for (size_t ptr = 0; ptr < ctx->nlines; ++ptr) {
                // Evict empty lines
                if (strlen(ctx->lines[ptr].path) == 0) {
                        continue;
                }

                ctx->lines[idx] = ctx->lines[ptr];
                ++idx;
        }

        ctx->nlines = idx;
}

void
trim(char **strp)
{
        // left trim
        char *str = *strp;
        while (*str != '\0' && *str == ' ')
                str++;

        *strp = str;

        // right trim
        str += strcspn(str, " ");
        *str = '\0';
}

int
csv_lex(struct csv *ctx)
{
        char *ptr = ctx->base;
        ctx->nlines = str_count(ptr, '\n', ctx->size);
        ctx->lines = xcalloc(ctx->err, ctx->nlines, sizeof(struct csv_line));
        if (ctx->lines == NULL)
                return (-1);

        printf("Number of lines: %lu\n", ctx->nlines);

        // Each iteration matches parsing a line
        // ntoken = number of commas + 1
        // We NULL terminated the file as part of the open step, which allows us to use strsep
        // We aren't using array_new here because the table of string contains mmaped value
        //    hence these can't be freed by array_free.
        for (size_t line = 0; line < ctx->nlines; ++line) {
                ctx->lines[line].path = strsep(&ptr, "\n");
		trim(&ctx->lines[line].path);

                printf("[%lu] path: '%s'\n", line, ctx->lines[line].path);
        }

        printf("packing\n");
        csv_pack(ctx);
        printf("finished packing\n");

        return (0);
}

int
csv_parse(struct csv *ctx, struct nvc_jetson_info *info)
{
        struct csv_line line;

        if (jetson_info_init(ctx->err, info, ctx->nlines) < 0)
                return (-1);

        for (size_t i = 0; i < ctx->nlines; ++i) {
                line = ctx->lines[i];

		mode_t mode;
		if (file_mode(ctx->err, line.path, &mode) < 0)
			continue;

		if (S_ISREG(mode)) {
			info->libs[i] = xstrdup(ctx->err, line.path);
			if (info->libs[i] == NULL)
				return (-1);

			printf("[%lu] lib: '%s'\n", i, info->libs[i]);
		} else if (S_ISDIR(mode)) {
			info->dirs[i] = xstrdup(ctx->err, line.path);
			if (info->dirs[i] == NULL)
				return (-1);

			printf("[%lu] dir: '%s'\n", i, info->dirs[i]);
		} else if (S_ISBLK(mode) || S_ISCHR(mode)) {
			info->devs[i] = xstrdup(ctx->err, line.path);
			if (info->devs[i] == NULL)
				return (-1);

			printf("[%lu] dev: '%s'\n", i, info->devs[i]);
		} else if (S_ISLNK(mode)) {
			info->symlinks[i] = xstrdup(ctx->err, line.path);
			if (info->symlinks[i] == NULL)
				return (-1);

			printf("[%lu] symlink: '%s'\n", i, info->symlinks[i]);
		} else {
			log_infof("malformed line: %s", line.path);
			continue;
		}
        }

        jetson_info_pack(info, ctx->nlines);

        return (0);
}
