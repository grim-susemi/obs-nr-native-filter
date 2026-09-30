# Task2 host

The runner stages only allowlisted installed OBS 32.2.2 files into
out/task-2/host using minimal private copies (about 46 MB of core dependencies).
NTFS denied hardlinks from Program Files; that failed attempt is preserved.
Every staged DLL must have a valid OBS Project signature. Source hashes, file
IDs and attributes are checked before/after. Source ACLs/bytes never change.
The profile and native module are separate writable task-owned files.
No OBS application, user profile, private NR runtime or injection is loaded.
Named ready/action/exit events exist before the child starts. The host loads
real libobs, D3D11, the native module and the stock x264 encoder. Foundation
is not filter registration, inference, recording or NR quality evidence.
