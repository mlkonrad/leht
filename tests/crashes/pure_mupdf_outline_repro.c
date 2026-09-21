/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure-MuPDF reproducer for the outline-depth stack overflow: no Leht code,
 * just open a document and load its outline.
 *
 *   gcc -O2 pure_mupdf_outline_repro.c -o repro /usr/lib64/libmupdf.so
 *   ./repro deep.pdf
 */
#include <mupdf/fitz.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	fz_document *doc = NULL;
	fz_outline *outline = NULL;
	if (argc != 2 || !ctx)
		return 2;
	fz_register_document_handlers(ctx);
	fz_try(ctx)
	{
		doc = fz_open_document(ctx, argv[1]);
		outline = fz_load_outline(ctx, doc);
		printf("outline loaded\n");
	}
	fz_always(ctx)
	{
		fz_drop_outline(ctx, outline);
		fz_drop_document(ctx, doc);
	}
	fz_catch(ctx)
		printf("error: %s\n", fz_caught_message(ctx));
	fz_drop_context(ctx);
	return 0;
}
