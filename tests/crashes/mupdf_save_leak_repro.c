/* Pure MuPDF: does pdf_save_document leak, and does do_garbage matter?
 * No Leht code. */
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int garbage = argc > 2 ? atoi(argv[2]) : 3;
    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);

    pdf_document *doc = NULL;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, argv[1]);
        pdf_write_options opts = pdf_default_write_options;
        opts.do_garbage = garbage;
        opts.do_compress = 1;
        pdf_save_document(ctx, doc, "/dev/null", &opts);
        printf("saved with do_garbage=%d\n", garbage);
    }
    fz_catch(ctx) { printf("failed: %s\n", fz_caught_message(ctx)); }

    pdf_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return 0;
}
