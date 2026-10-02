# Ghidra / ReVa MCP notes

- Renaming a function that already has a custom primary name: use `set-function-prototype` with the new name in the signature (`createIfNotExists: false`) as the FIRST call — `create-label` + `setAsPrimary: true` can silently leave the label secondary, and that leftover secondary label then blocks `set-function-prototype` with "symbol already exists at this address" (no MCP tool deletes labels; recovery requires deleting the label in the GUI and re-running `set-function-prototype`).
