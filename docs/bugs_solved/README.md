# OHAO Engine - Solved Bugs Log

This folder documents significant bugs that have been identified and fixed in the OHAO Engine. Each bug is documented with:
- Symptom description
- Root cause analysis
- Solution implemented
- Files modified

## Bug Index

| ID | Date | Severity | Summary |
|----|------|----------|---------|
| [001](001_staging_buffer_resize.md) | 2026-01-17 | Critical | Multi-frame staging buffers not resized on viewport resize (blinking) |
| [002](002_shadow_descriptor_set_mismatch.md) | 2026-01-17 | High | Shadow pass using wrong descriptor set (90° shadow rotation) |

## Contributing

When fixing a significant bug, add a new file following the naming convention:
```
NNN_short_description.md
```

Include:
1. Date solved and severity
2. Symptom (what the user sees)
3. Root cause (technical explanation)
4. Solution (code changes made)
5. Files modified
6. Verification steps
