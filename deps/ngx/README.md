# NVIDIA NGX SDK files (not included)

The build needs the NGX SDK headers in this folder. NVIDIA SDK files are not
redistributed here. The plugin does not statically link an NGX SDK library:
the effects use the NVIDIA driver and separately installed runtime files.

The previously recorded SDK header inventory and local drop are in
[provenance/sdk-lock.json](../../provenance/sdk-lock.json).
