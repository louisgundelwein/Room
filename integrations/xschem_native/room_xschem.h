#ifndef ROOM_XSCHEM_H
#define ROOM_XSCHEM_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct rx_document rx_document;
typedef struct rx_error rx_error;
typedef struct { const char *data; size_t size; } rx_string;
/* Borrowed values: coordinates in Xschem units; polygon values are interleaved x/y.
 * C: x,y; T: x,y,sizeX,sizeY; A: x,y,radius,start,span; N/L/B: x1,y1,x2,y2.
 * Strings are Xschem string contents (escaped, no enclosing braces). */
typedef struct {
    char kind;
    int layer, rotation, flip;
    const double *values;
    size_t value_count;
    rx_string text, attributes;
} rx_record;
typedef int (*rx_record_callback)(void *, const rx_record *);

/* Every fallible call returns 1/0. Error ownership passes to the caller, even on open failure. */
int rx_open(rx_string path, rx_document **out, rx_error **error);
/* Read the sole schematic, or the one matching <cell>.schematic.room. */
int rx_read(rx_document *, rx_record_callback, void *, rx_error **);
/* Expected type for a supported reference spelling. This does not verify its loaded definition. */
const char *rx_value_symbol_type(rx_string symbol);
/* Stage the whole editor snapshot; only existing `value` attributes on editable devices may change.
 * Unique names captured by read are immutable identities. Attribute keys cannot change.
 * A failed append invalidates staging. A failed commit leaves the original file intact. */
int rx_begin(rx_document *, rx_error **);
int rx_append(rx_document *, const rx_record *, rx_error **);
/* Stage a C record whose linked symbol definition the editor has verified. */
int rx_append_verified_instance(rx_document *, const rx_record *, rx_error **);
int rx_commit(rx_document *, rx_error **);
void rx_close(rx_document *);
rx_string rx_error_message(const rx_error *);
void rx_error_free(rx_error *);
#ifdef __cplusplus
}
#endif
#endif
