/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure-MuPDF reproducer for quadratic page lookup: no Leht code. Loads and
 * bounds every page, as a viewer does on open, and prints the time taken.
 *
 *   gcc -O2 pure_mupdf_pagetree_repro.c -o repro /usr/lib64/libmupdf.so
 *   ./repro slow.pdf
 */
#include <mupdf/fitz.h>
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
	fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	fz_document *doc = NULL;
	struct timespec t0, t1;
	int i, n = 0;
	if (argc != 2 || !ctx)
		return 2;
	fz_register_document_handlers(ctx);
	fz_set_warning_callback(ctx, NULL, NULL);
	fz_set_error_callback(ctx, NULL, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	fz_try(ctx)
	{
		doc = fz_open_document(ctx, argv[1]);
		n = fz_count_pages(ctx, doc);
		for (i = 0; i < n; i++)
		{
			fz_page *page = fz_load_page(ctx, doc, i);
			(void)fz_bound_page(ctx, page);
			fz_drop_page(ctx, page);
		}
	}
	fz_always(ctx)
		fz_drop_document(ctx, doc);
	fz_catch(ctx)
		printf("error: %s\n", fz_caught_message(ctx));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("%d pages bounded in %.2f s\n", n,
		(double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
	fz_drop_context(ctx);
	return 0;
}
