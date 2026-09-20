/* Minimal pure-MuPDF reproducer: no leht code involved at all. */
#include <mupdf/fitz.h>
#include <stdio.h>

int main(int argc, char **argv) {
    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document *doc = NULL;
    fz_try(ctx) {
        doc = fz_open_document(ctx, argv[1]);
        printf("opened, %d pages\n", fz_count_pages(ctx, doc));
    }
    fz_catch(ctx) {
        printf("rejected cleanly: %s\n", fz_caught_message(ctx));
    }
    if (doc) fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    printf("exited normally\n");
    return 0;
}
