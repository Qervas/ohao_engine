---
module: agent
id: transports
title: Transports: stdio and the named pipe
standard: v2
---

## A byte stream and nothing else

The server reads and writes through a four-member interface: `read` blocks until at least one byte arrives and returns 0 at the end of input; `write` sends one whole line and flushes it; and `interruptRead` makes a blocked `read` on a given thread return 0. Framing, parsing and threading are the server's. A transport only moves bytes, so the same server runs over stdin and stdout, over a named pipe, and over the in-memory streams `rpc_tests` feeds.

{{cite ohao/rpc/transport.hpp "virtual void interruptRead(void* readerThread) { (void)readerThread; }"}}

The default does nothing. The server calls it in a loop until its reader has returned, so a transport that cannot interrupt only delays a shutdown; it never hangs one that the client ends.

## Framing: newline-delimited JSON

Spec 3 had planned LSP-style `Content-Length` headers for a future transport. Sub-project 4 corrected that: MCP's stdio transport is newline-delimited, and compact JSON never contains a raw newline, so a newline is a complete frame. `LineReader` splits the stream at each `\n` and drops one `\r` before it, so a client that writes `\r\n` is understood:

{{cite ohao/rpc/transport.cpp "if (len && data[m_begin + len - 1] == '\r') --len;"}}

It reads in 64 KB chunks into a buffer it reuses. It remembers how far it has already searched, so a long line arriving in many small reads is scanned once rather than once per read; `sp4_t07_crlf_and_reads_split_anywhere` feeds one request a byte per read and two more split at arbitrary points. A last line without a newline still counts as a line.

The limit on a message (`--max-message`, 64 MiB by default) is enforced without ever holding the oversize message. Once the pending bytes exceed the limit with no newline in sight, the reader discards them and enters a skipping state until the next newline:

{{cite ohao/rpc/transport.cpp "if (m_scanned > m_max) {   // too long already: keep nothing more of it"}}

The skipped line is reported once as `Oversize`, which the server answers with -32600, and the next line is read normally. A client that sends a gigabyte without a newline therefore costs the server at most the limit plus one chunk of memory.

## stdout belongs to the protocol

On stdio, any byte on stdout that is not a protocol message breaks the stream. Engine code never prints, because it logs through `OHAO_LOG` to stderr, and a lint keeps it that way; but Jolt, NRD, assimp and the Vulkan driver can all print. So `ohao` separates the two streams at the descriptor level before anything else runs:

{{cite tools/ohao/main.cpp "if (FILE* f = rpc::separateStdout()) g_json = f;"}}

`separateStdout` duplicates descriptor 1 into a private copy and then points descriptor 1 at stderr, so every later `printf`, `std::cout` or library print lands on stderr. On Windows `_dup2` redirects the console handle too. Only writes through the private copy reach the client. The copy is switched to binary first:

{{cite ohao/rpc/transport.cpp "_setmode(copy, _O_BINARY);   // a dup keeps the text mode, which would write \r\n"}}

In text mode every `\n` the server wrote would reach the client as `\r\n`. `StdioTransport` sets stdin to binary for the same reason, so `\r\n` reaches the line reader exactly as the client sent it, and it reads descriptor 0 with `_read` rather than through `stdin`'s buffer.

## Stopping a read nobody will finish

A window host can end while the client still holds stdin open: the user closes the window. The reader thread is then blocked in `_read`, and joining it would hang. `Server::stop()` calls `interruptRead` with the reader's native thread handle until the reader has returned, and on Windows that cancels the pending synchronous read:

{{cite ohao/rpc/transport.cpp "CancelSynchronousIo(static_cast<HANDLE>(readerThread));"}}

The cancelled `_read` returns an error, which the transport reports as the end of input, and the reader exits the way it does at EOF. `serveWindow` relies on this after the live host's loop returns:

{{cite tools/ohao/main.cpp "server.stop();   // the window closed while the client still holds stdin: stop reading"}}

On POSIX there is no equivalent, and the comment says a host closes stdin instead. `sp4_t16_stop_interrupts_a_read_left_open` covers the Windows path.

## The named pipe

stdio serves exactly one client: the parent process. A client that is not the server's parent (a second tool, a debugger, an editor attached to an engine a human started) needs a name to connect to. `--pipe NAME` serves `\\.\pipe\ohao-NAME` instead of stdio, on Windows only; elsewhere the option is a usage error. The name is 1 to 64 characters of `[A-Za-z0-9._-]`, checked before anything is created. There is no TCP or WebSocket listener, not even behind a flag: a local pipe with an access check is the only way into the engine besides inheriting its stdio.

The pipe's security descriptor is built from the current user's SID as a protected DACL with a single entry:

{{cite ohao/rpc/transport.cpp "std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";"}}

`P` marks the DACL protected, so no entry is inherited, and the one `A` entry grants generic access to the current user. Without an explicit DACL, a pipe gets the default one, which gives Everyone read access. The pipe is created with three more restrictions:

{{cite ohao/rpc/transport.cpp "HANDLE pipe = CreateNamedPipeW(wpath.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,"}}

{{cite ohao/rpc/transport.cpp "PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,"}}

The instance count is 1, so a second client gets `ERROR_PIPE_BUSY`. `FILE_FLAG_FIRST_PIPE_INSTANCE` turns a name that another process already serves into an error. Without the flag, the server would quietly become a second instance next to a squatter that could take its clients. `PIPE_REJECT_REMOTE_CLIENTS` refuses clients that come through the SMB redirector. The step-10 record measured this: `\\localhost\pipe\…`, `\\127.0.0.1\pipe\…` and `\\<hostname>\pipe\…` each failed with `ERROR_ACCESS_DENIED`. A control pipe with the same DACL but without the flag admitted the `\\localhost` client, so the refusal comes from the flag, not the DACL.

`cmdServe` creates the pipe before the window, the renderer or the scene. A bad name or a taken one therefore fails at once, with exit 1 and `E_COMMAND` on stdout, instead of after a GPU start-up:

{{cite tools/ohao/main.cpp "auto pipe = rpc::PipeTransport::create(opt(a, "--pipe"), pipeError);"}}

## Overlapped I/O and the stop event

The pipe is opened for overlapped I/O, so every wait can be ended from another thread. The first `read` waits for a client in `ConnectNamedPipe`, and every later `read` waits in `ReadFile`. Each wait covers two events, the operation's own and the transport's manual-reset stop event:

{{cite ohao/rpc/transport.cpp "HANDLE events[2] = {ov->hEvent, m_stop};"}}

`interruptRead` sets the stop event. The waiting call cancels its operation with `CancelIoEx` and waits for the cancellation to complete before its `OVERLAPPED` goes out of scope. The read then returns 0. This is what lets a window that nobody ever connected to close cleanly: the reader is still blocked in `ConnectNamedPipe` when the user closes it (`sp4_t15_stop_ends_a_wait_for_a_client`).

A read that completes with zero bytes is not treated as the end of input: a client may write zero bytes. The end is a failed read, usually `ERROR_BROKEN_PIPE` when the client closes its handle:

{{cite ohao/rpc/transport.cpp "if (n > 0) return n;   // a zero-byte write by the client is not the end"}}

## One client per process

The spec first said "one client at a time". As built, the server takes one client per process. The client's disconnect is the end of input, exactly as EOF on stdin is: the running request is cancelled, the session ends, and the process exits 0. stdin is not read while serving a pipe. A failed write also marks the pipe closed, so the reader sees the end of input on its next read instead of the server writing into a dead handle. If the process exits while the client is still connected, the transport flushes before it disconnects, so the client reads the last replies before it sees the end:

{{cite ohao/rpc/transport.cpp "FlushFileBuffers(m_pipe);   // the client reads the last replies before it sees the end"}}

stderr carries `serving on <path>` and, once a client arrives, `a client connected to <path>`. `rpc_tests` covers the pipe in-process: one local client served, a second refused, a second server on the same name refused, the DACL read back with `GetSecurityInfo`, remote clients refused, and a stopped wait. `check_serve.py` covers it as a process with stdin left open throughout, and `check_live.py` drives the window host over the pipe.

:::why
The spec's security section starts from "no listener by default": stdio serves exactly the parent process, and sub-project 4 has no port at all. When a second way in was needed, a named pipe was chosen because the operating system enforces who may open it. The pipe carries a security descriptor, and a flag rejects network clients. With a protected one-entry DACL, a single instance and the first-instance flag, the only process that can open it is one running as the same user on the same machine.
:::

:::key
Lines end with `\n` (a `\r` before it is dropped), and a line over the limit is skipped without being kept. stdout belongs to the protocol: descriptor 1 points at stderr and the protocol writes through a private binary copy. `--pipe NAME` gives one local client of the current user (protected DACL, one instance, first-instance flag, remote clients rejected), whose disconnect ends the session. Every blocking read can be interrupted, so closing the window always ends the process.
:::

## Contracts

- `separateStdout()` must run before anything can print, which means first thing in `main`. If the duplication fails, nothing is redirected, and logs share stdout with the protocol.
- `Transport::write` is never called from two threads at once: the server serializes writes under its own mutex.
- A `PipeTransport` serves one client and is not reused; the process exits when that client leaves.
- `--pipe` changes only the transport. Paths, caps, time modes and both hosts behave exactly as over stdio.
