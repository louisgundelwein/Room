# Review

## Repeated findings

- Native Xschem parameter saves must preserve the editor's decoded value through save and reopen or refuse before publishing. Compare the real editor value and ROOM property bytes independently of the adapter parser. On refusal, verify the original file bytes and dirty edit are retained. Cover quoted backslashes, unquoted escapes and carriage returns.
- A native Xschem value edit requires the linked symbol's verified standard definition. The requested reference and declared type alone cannot authorize a write. Exercise both bare and `devices/` search-path shadows with the standard type, and confirm refusal preserves file bytes and the dirty edit.
