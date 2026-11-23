# Native Xschem parameters

Enable `ROOM_BUILD_XSCHEM_NATIVE=ON` on Linux to build and install
`libroom_xschem.so` and `room_xschem.h`. The C adapter uses the existing
ROOM and Cap'n Proto libraries.

The editor opens a binary ROOM file as typed records. It selects the sole
schematic or the cell named by `<cell>.schematic.room`. The schematic must
be flat and use external device symbols. Ambiguous instance names,
malformed native metadata and unsupported geometry or transforms fail
before record delivery.

The editor stages a complete drawing. Only an existing `value` on `res.sym`,
`capa.sym` or `ind.sym` (also under `devices/`) is editable. The editor checks
the symbol's declared type. Labels, pins, directives, custom device parameters
and other attributes are read-only. The `value` must have exactly one
original ROOM property.

Use double quotes for spaces and for literal backslashes or double quotes.
Inside quotes use `\\` for a literal backslash and `\"` for a literal quote.
Ambiguous unquoted spellings and values that cannot be reopened exactly are
refused. Carriage returns refuse because Xschem's disk undo reader strips them.

Save patches a copy of the original binary at those property values and
updates existing native attribute metadata when needed. Explicit nets,
unknown fields and unrelated properties, cells and views are carried
through the patch. The temporary must decode to the requested values.
Reversing only the authorized value patches must reproduce the entire
original message canonically. A no-op checks the opened revision and
skips writing.

A persistent sibling `.lock` serializes cooperating publishers. Saving
requires a writable regular target with one hard link. The adapter checks
the original bytes and resolved path, writes and flushes an exclusive
sibling temporary, validates it, rechecks the revision and renames it
atomically. A refusal leaves the original and editor edits available.
Non-cooperating writers can still race the final check and rename. The
prototype assumes a local Linux filesystem and does not promise
power-loss durability or interoperability with other writer locks.

`xschem_native` covers the typed projection, ordinary and compact storage,
exact values, preservation, refused edits, stale revisions, a waiting writer
and an injected write failure. The Xschem integration README has the
isolated build and editor review commands.
