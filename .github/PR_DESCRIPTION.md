## Fix UTF-8 validation error on binary payload data (v0.1.1)

### Problem
Querying streams with binary data (e.g., protobuf) without extraction parameters caused UTF-8 validation errors:
```sql
SELECT * FROM nats_scan('telemetry') LIMIT 10;
-- Error: Invalid unicode (byte sequence mismatch)
```

### Solution
Return `payload` column as BLOB instead of VARCHAR when no extraction parameters specified.

### Changes
- `src/nats_scan.cpp`: Use BLOB type for payload when `json_extract` not specified
- Added test suite validating BLOB behavior

### Testing
- All existing tests pass
- New tests validate BLOB type and UTF-8 fix

