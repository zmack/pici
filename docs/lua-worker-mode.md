# Sandboxed Lua-only mode

Run Pici with one model-visible tool, `lua`, backed by a disposable Lua 5.4
process inside bubblewrap:

```sh
./build/pi-cli --lua-only
```

This requires Linux with working unprivileged user namespaces, util-linux
`prlimit`, and bubblewrap supporting descriptor/data mounts and
`--disable-userns`. Keep `pi-lua-worker` beside `pi-cli`; CMake builds both.
The worker links
the system Lua 5.4 library selected by pkg-config and bundled LuaFileSystem
1.8.0; no separate `lua` command or LuaRocks installation is needed.

The workspace is read-only by default. To allow edits:

```sh
./build/pi-cli --lua-only --lua-write-root .
```

For narrower access, writable roots must be existing directories within the
current workspace:

```sh
./build/pi-cli --lua-only --lua-write-root src --lua-write-root scratch
```

`--lua-library io --lua-library lfs` selects a session library ceiling.
The mode is selected at startup, rather than inferred from a resumed transcript;
keep `--lua-only` in the command or set it in configuration.

`--lua-read-root dir` replaces the default read root (`.`) and is repeatable.
Write roots grant read access to those roots as well. All root paths are
resolved against the startup workspace; roots outside it are rejected.

Equivalent configuration:

```toml
[tools]
mode = "lua-only"

[lua]
libraries = ["io", "os", "lfs"]

[lua.sandbox]
read_roots = ["."]
write_roots = ["src", "scratch"]
network = false
address_space_mb = 1024
```

Explicit empty arrays are respected. For a worker with no mounted project
files, set both root arrays to `[]`. Read-only system runtime directories
remain available; “no project mount” is not a claim that no system files
exist. Network access is always off in this release; `network = true` is
rejected. Unknown Lua configuration options are errors.

Lua-only mode conflicts with `--no-tools`, `--no-builtin-tools`, `--lua-eval`,
`--sandbox disabled`, faux-control mode, and tool allowlists other than
`lua`. ACP does not support this mode. The ordinary in-process `--lua-eval`
mode and existing bash tools retain their behavior. The worker never falls
back to unsandboxed execution, including after resuming older sessions.
Add-on tool registration/reload cannot expand the worker-only tool set;
trusted add-on code itself still runs in the host.

## Tool calls

The model supplies explicit imports outside the script:

```json
{
  "requires": ["io", "lfs"],
  "script": "local io=require('io'); local lfs=require('lfs'); local f=assert(io.open('README.md')); local text=f:read('*a'); f:close(); return {cwd=lfs.currentdir(), text=text}",
  "cwd": ".",
  "timeout_ms": 30000
}
```

`requires` is mandatory, including `[]` for pure computations. It may request
only `io`, `os`, and `lfs` within the configured ceiling. Unselected modules
are absent from globals and cannot be imported. No unrestricted package
loader, debug library, `loadfile`, or `dofile` is exposed. Lua base utilities,
string patterns, table, math, utf8, coroutine, and `json.encode/decode` are
available without imports. `json.null` represents JSON null. Dense tables
encode as arrays; unmarked empty tables encode as objects. Decoded arrays retain
their JSON type; use `json.array()` to construct an empty JSON array. JSON values must have string
object keys, finite numbers, at most 32 nesting levels, and at most 4096
entries per table. Functions, threads, userdata, cycles, and invalid UTF-8
return values are errors.

Use `mode: "check"` to compile without execution or loading effectful modules.
A successful check does not prove runtime imports or file/command operations
will succeed. Use `io.open` for text/binary files, `lfs.dir`/`chdir` for
directories, and `os.execute`/`io.popen` for commands. Child stdout/stderr is
captured separately from the structured return values. `os.exit` ends only
the worker and can result in a missing-result error.

## Permissions and limits

`requires` selects interfaces, not filesystem/network grants. Full `io`
includes `popen`; full `os` includes shell execution. Scripts may launch shell,
Python, or other installed programs within their sandbox. Shell and native
library operations share the same read/write mounts and network namespace.
Omitting a library is not a separately enforced syscall prohibition.

The worker has read-only `/usr`, `/bin`, `/sbin`, `/lib`, and `/lib64` mounts
where present, but no host `/etc` or HOME. Installed commands and their runtime
dependencies must be available there. Commands needing host configuration,
credentials, network downloads, or unmounted files will fail. The worker executable is snapshotted and sealed when this process first
admits a Lua worker tool. Rebuilding or replacing the on-disk worker takes
effect after restarting Pici; workspace writes cannot replace the active image.
The environment
starts empty and receives only controlled PATH, HOME, TMPDIR, and locale.
Mount sources are pinned with descriptors and workspace-relative roots are
opened using Linux `openat2` containment checks. Root replacement by a symlink
fails rather than widening the policy.

Every mounted project file is visible within its configured scope, including
`.env`, `.git`, secrets, hard-link aliases, and Unix sockets. There is no
per-file exclusion/masking configuration in this first release. Mount only
roots whose contents you intend to expose; read-only mounts still allow
reading. Runtime directories are visible in full. Host-side Pici provider
credentials/environment/descriptors are not passed into the worker.

Each invocation starts fresh at `/workspace` or its explicit relative `cwd`.
`lfs.chdir` changes only that worker's directory. Globals, functions, cwd,
temporary files, and live file handles do not persist. Mounted workspace
changes do persist and may conflict with other sessions. Host `pici` session,
agent, mailbox, and scratch bindings are not provided by this backend.

Parent-side deadlines default to 30 seconds (maximum 120 seconds), cover
blocking/native work, and terminate the worker namespace/process tree.
Remaining descendants are removed on normal completion as well. Combined
stdout/stderr is capped at 512 KiB, requests at 128 KiB, source at 64 KiB,
and structured results at 1 MiB. Invalid bytes in stdout/stderr are replaced
when encoded into the tool result; binary data should be encoded explicitly.

The launcher installs hard per-process limits before executing the worker.
The worker also verifies per-process address-space (1024 MiB by default), CPU (120 seconds),
file-size (256 MiB), descriptor (128), and per-user process-count (256) limits;
these are inherited by commands. Temporary storage is a separate 64 MiB tmpfs.
`address_space_mb` accepts 64–65536, or 0 to explicitly disable the address-space
limit. Programs with large virtual-memory reservations, including some JS
runtimes, may need a larger setting or 0. ASan/TSan builds also need 0 for
shadow-memory reservations. This setting changes no filesystem/network grants.

These limits do not constitute aggregate memory/CPU quotas for all descendants
or a total quota for writes to mounted workspace files. Process-count limits
are per-user kernel limits, not a dedicated per-invocation process budget.

Failures and cancellation do not undo previous writes or commands. Tool
results show status, output, requested libraries, and sandbox policy; they are
not a complete audit of every file read, write, or syscall. No automatic
retry or transaction semantics are supplied.

The tool definition stays identical across profiles and imports. A compact,
activation-stable policy snapshot accompanies the system instructions; import
choices remain ordinary call arguments. There is no schema regeneration per
call. LPeg, argv-based process convenience APIs, extra parsers, networking,
persistent workers, and host coordination are follow-up features rather than
part of this initial runtime.
