# 20. Windows paths and files

This is what burrow does with a path or a file on Windows, written down before any of it is implemented, because Windows is where a port that copies Go's API and guesses at the behaviour goes wrong. Every rule here is Go's, taken from Go 1.27's `internal/filepathlite/path_windows.go`, `path/filepath`, `os/*_windows.go` and `syscall/syscall_windows.go`, and the section names the file so a reader can check. Where burrow cannot do what Go does, the section says so and says what it does instead.

The implementation lands with `os` and `path/filepath` in P2. Until then this document is the contract, and a change to it needs the same review as a change to the code would.

## 1. Strings at the boundary

burrow strings are UTF-8, and Windows file APIs take UTF-16. The conversion is Go's, in `syscall.UTF16FromString` and `UTF16ToString`:

- Going in, a NUL byte anywhere in the string is `EINVAL` and nothing is called. A path with an embedded NUL must never reach `CreateFileW` truncated.
- Going in, bytes that are not valid UTF-8 become U+FFFD, one per bad byte, with one exception. A three byte sequence that encodes a lone surrogate is WTF-8 and turns back into that surrogate, so a name that came out of Windows goes back in unchanged.
- Coming out, an unpaired surrogate is written as its WTF-8 bytes rather than replaced. Windows file names are sequences of 16 bit units with no requirement to be valid UTF-16, and replacing a lone surrogate with U+FFFD would give back a name that opens nothing.

Every PAL entry point that takes a path does the conversion itself, into a stack buffer when the result fits in `MAX_PATH` units and from the call's allocator when it does not. Nothing above the PAL sees a `wchar_t`. burrow never calls the `A` form of a Windows function, because those go through the ANSI code page and lose any character outside it.

## 2. What a path looks like

`filepath.Separator` is `\` and `ListSeparator` is `;`. Both `\` and `/` are separators everywhere a path is read, and `Clean`, `Join` and `FromSlash` write `\`.

The volume name is the part of a path before its first separator that belongs to the filesystem rather than to a directory. `VolumeName`, `IsAbs`, `Clean`, `Split`, `Rel` and `EvalSymlinks` all agree on it, because they all call the same function, `volumeNameLen`, which recognises:

| Form | Example | Volume |
|---|---|---|
| Drive letter | `C:\a`, `C:a` | `C:` |
| UNC | `\\host\share\a` | `\\host\share` |
| Local device | `\\.\COM1`, `\\.\C:\a` | `\\.\COM1`, `\\.\C:` |
| Root local device | `\\?\C:\a`, `\??\C:\a` | `\\?\C:`, `\??\C:` |
| UNC through a device prefix | `\\?\UNC\host\share\a` | `\\?\UNC\host\share` |

The details that matter:

- A drive letter is any byte followed by `:`. Go does not check that it is a letter, and neither does burrow.
- `C:a` is relative to the current directory on drive C, not absolute. `IsAbs` is true only for a volume followed by a separator, or for any path that starts with two separators.
- `\a` is rooted in the current drive and is not absolute either.
- The device prefixes are compared without regard to case and with either separator, so `//?/c:/a` is the same as `\\?\C:\a`.
- The component after a device prefix is part of the volume, so ``Clean(`\\?\c:\`)`` keeps its trailing separator. That is Go issue 64028.
- A volume containing a `..` component is not a volume. `volumeNameLen` returns 0 for it, which stops `\\a\..\b` being read as a share called `..`.

## 3. Clean and Join

`Clean` is the same lexical algorithm as on Unix, applied after the volume, followed by a fix-up that Unix does not need. Go calls it `postClean`, and it exists because two relative paths can clean into something that is not relative:

- If the first element of a relative result contains a `:`, `Clean` puts `.\` in front. Otherwise `a/../c:` would clean to `c:`, which is a drive.
- If a relative result starts with `\??`, `Clean` puts `\.` in front. Otherwise `\a\..\??\c:\x` would become `\??\c:\x`, which is the NT path for `c:\x`.

`Join` has the same guard. When it joins onto a lone separator and the next element starts with `??`, it inserts `.\`. It does not add a separator after an element ending in `:`, so `Join("C:", "a")` is `C:a`, the relative path, and not `C:\a`.

`SplitList` understands double quotes, because `PATH` entries on Windows may be quoted to hold a `;`. Quotes are removed from the results. An empty string gives an empty list.

`HasPrefix` compares case-insensitively. It is deprecated in Go and kept in burrow for the same reason Go keeps it.

## 4. Names Windows gives away

Some names do not name files. `CON`, `PRN`, `AUX`, `NUL`, `COM1` to `COM9` and `LPT1` to `LPT9` open devices wherever they appear in a path, in any case, with any extension on older Windows, and with trailing spaces ignored. `COM` and `LPT` followed by a superscript ¹, ² or ³ are reserved too, and so are `CONIN$` and `CONOUT$`, which open the console.

This matters for security, not tidiness. `filepath.IsLocal` and `filepath.Localize` are what a program uses to check that an untrusted name stays inside a directory, and a name like `NUL` or `COM1.txt` fails that check even though it has no separators and no `..`. Go has had CVEs here (CVE-2022-41722 and CVE-2023-45283), and burrow's versions of these functions are ported line for line, not re-derived.

- `isReservedName` cuts the element at the first `.` or `:`, trims trailing spaces, and matches the base against the list. A bare name like `nul` is reserved on every Windows. A name with an extension, like `con.txt`, stopped being reserved in Windows 11, so for those Go asks the system with `RtlIsDosDeviceName_U`, and burrow does the same.
- `IsLocal` is false for an empty path, anything starting with a separator, anything containing a `:` anywhere, any element that is a reserved name, and anything that cleans to `..` or starts with `..\`.
- `Localize` rejects `:`, `\` and NUL and any reserved element, and turns `/` into `\`.

The fuzz target for these functions is seeded with the inputs from Go's tests for both CVEs, and it runs in the differential fuzzer against Go itself, since these are the functions where a disagreement with Go is most likely to be exploitable.

## 5. Long paths

The Win32 path limit is `MAX_PATH`, 260 units, unless the process says otherwise. Go gets around it in two ways, and burrow does both.

At startup the Go runtime asks `RtlAreLongPathsEnabled`, and if the machine allows long paths, it sets `IsLongPathAwareProcess` in the PEB. That flag is what an application manifest would set. After that, Win32 takes long paths as they are. burrow's runtime does the same thing in its Windows init, because a library cannot rely on the program having the manifest entry.

When long paths are not enabled, every `os` function puts the path through `fixLongPath` before it reaches the system. That function:

- Returns the path unchanged if it already starts with `\\?\` or `\??\`.
- Returns it unchanged if it is shorter than 248 bytes, counting the current directory for a relative path. The limit is 248 rather than 260 because creating a directory must leave room for an 8.3 name. Go keeps the working directory in a cache so that it does not call `GetCurrentDirectory` every time, and burrow does too.
- Returns a `\\.\` device path unchanged, since a prefix would change what it means.
- Otherwise makes the path absolute with `GetFullPathNameW` and prefixes `\\?\`, or `\\?\UNC\` for a UNC path.

Relative symlink targets are the one exception. `Symlink` does not fix a relative target, because making it absolute would create a different link.

## 6. Opening a file

`OpenFile` maps Go's flags onto `CreateFileW` the way `syscall.Open` does, and the choices are not obvious:

| Flag | What burrow asks for |
|---|---|
| `O_RDONLY`, `O_WRONLY`, `O_RDWR` | `GENERIC_READ`, `GENERIC_WRITE`, or both |
| `O_CREAT` | adds `GENERIC_WRITE`, and `OPEN_ALWAYS` |
| `O_CREAT` and `O_EXCL` | `CREATE_NEW`, with `FILE_FLAG_OPEN_REPARSE_POINT` so a symlink at the name is not followed |
| `O_TRUNC` | truncates after opening, and not when the file was just created |
| `O_APPEND` | every right in `GENERIC_WRITE` except `FILE_WRITE_DATA`, unless `O_TRUNC` also needs it |
| `O_SYNC` | `FILE_FLAG_WRITE_THROUGH` |
| no `O_CLOEXEC` | an inheritable handle |
| a mode without `0200` | `FILE_ATTRIBUTE_READONLY` on a file it creates |

Why these:

- `O_TRUNC` is never `CREATE_ALWAYS` or `TRUNCATE_EXISTING`. `CREATE_ALWAYS` on a file with the read-only attribute replaces it with a new read-only file (Go issue 38225), so Go opens the file and truncates it itself.
- `O_APPEND` removes `FILE_WRITE_DATA` because that is what makes Windows append, and it cannot simply clear that one bit from `GENERIC_WRITE`: a handle with `GENERIC_WRITE` and no `FILE_WRITE_DATA` writes at offset zero.
- A read-only open always adds `FILE_FLAG_BACKUP_SEMANTICS`, which is what lets `CreateFileW` open a directory. A write open never does, so that opening a directory for writing fails, and burrow turns that `ERROR_ACCESS_DENIED` into `EISDIR` after checking that the name is a directory, the same as Go.
- The share mode is `FILE_SHARE_READ | FILE_SHARE_WRITE` and never `FILE_SHARE_DELETE`. So a file that is open anywhere in the process cannot be removed or renamed, which is the largest difference a Unix programmer will meet. Go has declined to change this (issue 32088) because it changes what other programs see, and burrow follows Go.
- The top twelve bits of the flag argument are passed to `CreateFileW` as `FILE_FLAG_*` values when they are ones Go allows, such as `FILE_FLAG_OVERLAPPED`, and are `EINVAL` otherwise.

The handle that `OpenFile` returns is a handle, not a file descriptor, and burrow's `OsFile` holds a `PalHandle` for that reason. No CRT file descriptor is ever created, and nothing in burrow calls `_open`, `_wopen` or `fopen` on Windows.

## 7. Modes and Stat

Windows does not have Unix permission bits, so `FileMode` on Windows is made up from the attributes, by Go 1.23's rules in `os/types_windows.go`:

- The permission bits are `0444` when `FILE_ATTRIBUTE_READONLY` is set and `0666` when it is not. A directory adds `0111`.
- A reparse point that stands in for another name, such as a symlink or a junction, gets no `ModeDir`, even when Windows sets the directory attribute on it. So `Lstat` followed by `IsDir` does not walk into a link, the same as on Unix.
- `IO_REPARSE_TAG_SYMLINK` is `ModeSymlink`. `IO_REPARSE_TAG_AF_UNIX` is `ModeSocket`. `IO_REPARSE_TAG_DEDUP` is a regular file, because the Data Deduplication service turns ordinary files into those at any time. Any other reparse tag, junctions included, is `ModeIrregular`.
- A handle of type `FILE_TYPE_PIPE` is `ModeNamedPipe`, and `FILE_TYPE_CHAR` is `ModeDevice | ModeCharDevice`.

Before Go 1.23 a junction was `ModeSymlink`, and Go still offers that behaviour through `GODEBUG=winsymlink=0`. burrow implements only the current rule. It has no `GODEBUG`, and the old rule is documented as a difference.

`Chmod` sets or clears `FILE_ATTRIBUTE_READONLY` from the `0200` bit and ignores every other bit, including on directories, where the attribute means nothing to Windows. `Chown` and `Lchown` return `EWINDOWS`, which is `errors.ErrUnsupported`.

`Stat` tries `GetFileAttributesExW` first because it is much cheaper than opening the file. It falls back to `FindFirstFileW` when that fails with `ERROR_SHARING_VIOLATION`, which is what `c:\pagefile.sys` does, and it opens the file with `FILE_FLAG_BACKUP_SEMANTICS` and reads the reparse tag only when the attributes say it is a reparse point. `ModTime` is `LastWriteTime` at its full 100 nanosecond resolution. `SameFile` compares the volume serial number and the 64 bit file index, and fetches those lazily, since most callers never ask.

## 8. Removing and renaming

`Remove` does not know whether the name is a file or a directory, so it tries `DeleteFileW` and then `RemoveDirectoryW`. If both fail, it picks which error to report by looking at the attributes: the directory error for a directory, and for a read-only file it clears the attribute and tries `DeleteFileW` once more. So `Remove` deletes a read-only file on Windows just as `unlink` does on Unix, where the file's mode never mattered.

`Rename` is `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`. It replaces a file atomically on the same volume. It fails with `ERROR_ACCESS_DENIED` when the target is a directory, and with `ERROR_NOT_SAME_DEVICE` across volumes, and both errors reach the caller unchanged inside a `LinkError`. Go does not fall back to a copy and neither does burrow.

`RemoveAll` works relative to an open directory handle, the same code as on Unix, with Go's `windows.Deleteat` standing in for `unlinkat`. That opens each name with `NtOpenFile` under the parent's handle, sharing read, write and delete, and deletes it with `FileDispositionInformationEx` and the flags `FILE_DISPOSITION_POSIX_SEMANTICS` and `FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE`, which remove the name at once even while another handle has it open and ignore the read-only attribute. When the system or the filesystem refuses those flags, which FAT and Windows before 10 1809 do, it falls back to the older disposition call and clears the read-only attribute itself. So `RemoveAll` can remove files that `Remove` cannot, and burrow keeps that difference because Go has it.

## 9. Links

`Symlink` has three rules that come from Windows:

- The target has `/` changed to `\` first, because a link's stored target is read by the kernel and `/` does not work there.
- A directory link and a file link are different objects. `Symlink` stats the target, resolving a relative target against the link's directory and a rooted one against the link's volume, and asks for `SYMBOLIC_LINK_FLAG_DIRECTORY` when it is a directory. A link to a name that does not exist yet is a file link and stays one, so it breaks if a directory appears there later. Go documents this, and burrow's documentation says it too.
- It asks for `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`, which works without administrator rights when Developer Mode is on, and if that fails it tries again without the flag, for Windows before 10 1703.

`Readlink` opens the name with `FILE_FLAG_OPEN_REPARSE_POINT`, reads the reparse data with `FSCTL_GET_REPARSE_POINT`, and returns a relative target as stored. It turns absolute NT paths back into Win32 ones: `\??\C:\a` becomes `C:\a`, `\??\UNC\h\s` becomes `\\h\s`, and a volume GUID path becomes `\\?\Volume{...}\`. It reads junctions as well as symlinks. Any other kind of reparse point is `ENOENT`, as in Go.

`Link` is `CreateHardLinkW`. `EvalSymlinks` walks the path one element at a time with `Readlink`, the same walk as on Unix, and then rewrites each element in the case the filesystem stored it, using `FindFirstFileW` on each prefix, and the drive letter in upper case. So two spellings of one file give the same result, which is what callers compare it for.

## 10. Reading directories

`ReadDir` uses `GetFileInformationByHandleEx` with `FileIdBothDirectoryInfo`, falling back to `FileFullDirectoryInfo` on filesystems that refuse the first. The first one includes the file index, so `SameFile` on a `DirEntry`'s info does not need another open. Both give the attributes and reparse tag, so a `DirEntry`'s type is known without a stat, and burrow never calls `FindFirstFileW` for a listing. `.` and `..` are dropped, and names come out in the order the filesystem gives, which on NTFS happens to be sorted and on FAT is not. `os.ReadDir` sorts them anyway.

## 11. Matching and Glob

On Windows `\` is a separator, so it cannot escape in a pattern. `filepath.Match` treats it as an ordinary character there, `hasMeta` does not count it, and a pattern that uses `\` to escape `*` on Unix matches differently on Windows. Tests that check `Match` are run with both rules. `Glob` also handles a volume at the front of the pattern, so `C:\*` globs the root of C and does not try to match the `C:`.

## 12. Environment, working directory and temporary files

Go on Unix keeps its own copy of the environment. On Windows it does not: `Getenv` calls `GetEnvironmentVariableW`, `Setenv` calls `SetEnvironmentVariableW`, and `Environ` reads `GetEnvironmentStringsW`, so the environment belongs to the process and matches what a child would see. burrow does the same on Windows, which corrects the line in [10](10-packages-os.md) §4 that says it keeps a copy everywhere. Names are compared without regard to case by Windows itself. `Environ` includes the hidden `=C:=C:\dir` entries that record each drive's current directory, because Go's does. `exec.Cmd` sorts a child's environment by upper-cased name before passing it to `CreateProcessW`, which is what Windows expects.

`TempDir` is `GetTempPath2W` where it exists (Windows 11 and Server 2022, where it gives SYSTEM processes their own directory) and `GetTempPathW` otherwise, with the trailing `\` removed unless the result is a drive root. `UserHomeDir` is `%USERPROFILE%`. `Getwd` is `GetCurrentDirectoryW`, and `Chdir` updates the cache from §5.

## 13. Processes

`os/exec` is in [10](10-packages-os.md) §8, and two parts of it are about paths.

The command line is one string on Windows, and the child splits it. burrow builds it with Go's `EscapeArg` and nothing else. An argument with no space, tab, quote or backslash goes through as it is. An empty argument is `""`. An argument with a space or tab is quoted. A quote is escaped with a backslash, and a run of backslashes is doubled when a quote follows it, including the closing quote that burrow adds. This is the convention `CommandLineToArgvW` and the Microsoft C runtime undo. Programs that split their own command lines differently, `cmd.exe` among them, need `SysProcAttr.CmdLine`, which passes a string through untouched, and the documentation says that the quoting is not a defence against `cmd.exe` metacharacters.

`LookPath` tries each extension in `%PATHEXT%`, falling back to `.com;.exe;.bat;.cmd` when it is unset, and does not add an extension when the name already has one of them. It searches the current directory first, the way Windows does, unless `NoDefaultCurrentDirectoryInExePath` is set, and a program it finds that way comes back with `ErrDot`, which is the Go 1.19 fix for running whatever was sitting in the directory.

## 14. Errors

Every `ERROR_*` code a Windows call returns reaches the caller as a `syscall.Errno` with that value, and `errors.Is` maps them to the `fs` errors with Go's table:

| Sentinel | Windows codes | Also |
|---|---|---|
| `ErrNotExist` | `ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND`, `ERROR_BAD_NETPATH` | `ENOENT` |
| `ErrExist` | `ERROR_ALREADY_EXISTS`, `ERROR_FILE_EXISTS`, `ERROR_DIR_NOT_EMPTY` | `EEXIST`, `ENOTEMPTY` |
| `ErrPermission` | `ERROR_ACCESS_DENIED` | `EACCES`, `EPERM` |
| `ErrUnsupported` | `ERROR_NOT_SUPPORTED`, `ERROR_CALL_NOT_IMPLEMENTED` | `ENOSYS`, `ENOTSUP`, `EOPNOTSUPP`, `EWINDOWS` |

`ERROR_SHARING_VIOLATION` and `ERROR_LOCK_VIOLATION` map to none of them, as in Go, even though the first is what a Unix programmer will hit most often after §6. The `E` constants on Windows are Go's invented values, which start at `APPLICATION_ERROR` so they cannot collide with a real Windows code, and `Error()` on a real code is the system's message from `FormatMessageW`, such as "The system cannot find the file specified."

That needs one change to the PAL. `pal.h` today reduces every failure to a `PalErrno` and drops the native code on purpose, which is right for the runtime and wrong for `os`, since a `PathError` on Windows has to hold the Windows code and print its message. The file, process and directory groups of the PAL, which are not written yet, will report the native code alongside the `PalErrno`, and `os` builds its `syscall.Errno` from that. The runtime's own calls keep using the `PalErrno` alone.

## 15. The console

When standard input or output is a console, bytes are not what Windows reads or writes. `os.Stdout` on a console writes with `WriteConsoleW`, converting UTF-8 to UTF-16 and holding back an incomplete sequence at the end of a write until the next one. `os.Stdin` reads with `ReadConsoleW` and converts back. A Ctrl-Z ends the read at that point, and one at the very start of a read is end of file. Anything that is not a console is read and written as bytes. This is why `printf` of UTF-8 to a Windows console shows mojibake and `fmt.Println` does not, and burrow's `fmt` behaves like Go's, not like the CRT's.

## 16. What burrow leaves out

- `GODEBUG=winsymlink=0` and `GODEBUG=winreadlinkvolume=0`. They bring back pre-1.23 behaviour, and burrow has no pre-1.23 behaviour to bring back.
- The `A` code page functions, as §1 says, and any use of the CRT's POSIX layer.
- Windows before 10. Go 1.27 needs Windows 10 or Server 2016, and so does burrow, which lets the PAL call `GetFinalPathNameByHandleW`, `WaitOnAddress` and `GetFileInformationByHandleEx` without looking them up first.

## 17. How this gets tested

Go's `path/filepath/path_windows_test.go`, `os/os_windows_test.go`, `os/path_windows_test.go` and `syscall/exec_windows_test.go` are translated with `burrow-gen tests` and run on windows/amd64 in CI and on gamingpc. The lexical parts of §2 to §4 and §11 do not touch the system, so they are also built on Linux with the Windows rules compiled in and run in the differential fuzzer against Go built with `GOOS=windows`, which is how a disagreement shows up before anyone has to boot Windows.
