# Wine GitLab Issue / Merge Request Proposal

**Title:** `server: Respect reported_events for AFD_POLL_WRITE in poll_socket to prevent busy-spin`  
**Subsystem:** `server/sock.c`, `ntdll`, `afd`  
**Target:** [WineHQ GitLab](https://gitlab.winehq.org/wine/wine/-/merge_requests)  

---

## 1. Problem Description & Empirical Evidence

When running modern Windows applications utilizing asynchronous socket polling via `IOCTL_AFD_POLL` (such as Riot Games Keystone / LeagueClient runtime), `wineserver` and the application process experience severe CPU starvation caused by an unthrottled busy loop.

In our empirical IPC/AFD tracing on Arch Linux (kernel 6.17 / Proton-CachyOS / Wine upstream), thread `06f4` issued **9,669,461 calls to `IOCTL_AFD_POLL` (0x00012024) in 162 seconds**, averaging **53,773 syscalls per second**:

```text
284567.125:01a4:trace:file:NtDeviceIoControlFile (0x128,0x124,(nil),(nil),0x775f510,0x00012024,0xe2bce0,0x00000030,0xe2bce0,0x00000030)
...
[Top callers of IOCTL_AFD_POLL in 160s window]:
    2034 thread_05fc
    2329 thread_06ec
 9669461 thread_06f4  <-- 53,773 calls/sec
```

This busy-spin consumed 134% CPU on the client process and 22% CPU on `wineserver`, causing massive thread contention and delaying initialization of peer threads and inter-process communication.

---

## 2. Root Cause Analysis

In Windows NT Winsock and `afd.sys`, `FD_WRITE` / `AFD_POLL_WRITE` notifications are **edge-triggered / state-based**:
1. An `FD_WRITE` notification is posted when a connection is first established (or accepted).
2. It is subsequently posted only after a `send` / `sendto` operation fails with `WSAEWOULDBLOCK` (`STATUS_DEVICE_NOT_READY`), once internal buffer space transitions back to available.
3. If an application repeatedly polls a writable socket without having received `WSAEWOULDBLOCK` on an earlier write, Windows does not continuously fire write events.

Wine's `server/sock.c` already models this state machine for `WSAEventSelect` through `sock->reported_events`:
- Lines 245–252 describe how `reported_events` ensures events are not signaled repeatedly once consumed until a reset operation occurs.
- Line 4003–4005 correctly clears `AFD_POLL_WRITE` from `reported_events` when a `send` fails:
  ```c
  if (iosb->status != STATUS_SUCCESS)
  {
      /* send() calls only clear and reselect events if unsuccessful. */
      sock->pending_events &= ~AFD_POLL_WRITE;
      sock->reported_events &= ~AFD_POLL_WRITE;
      sock_reselect( sock );
  }
  ```

However, `IOCTL_AFD_POLL` (implemented in `poll_socket()`, `poll_flags_from_afd()`, and `get_poll_flags()`) completely ignores `sock->reported_events`:
1. `poll_flags_from_afd()` unconditionally sets `POLLOUT` if `flags & AFD_POLL_WRITE`.
2. Linux `poll()` with timeout 0 is level-triggered and reports `POLLOUT` on any socket with available buffer space (which is permanently true for connectionless/UDP sockets).
3. `complete_async_polls()` copies `req->sockets[i].mask & flags` to `req->sockets[i].flags`, setting `AFD_POLL_WRITE`.
4. `poll_socket()` observes `signaled = TRUE` and immediately completes the poll with `STATUS_SUCCESS` synchronously without waiting.
5. The calling application sees `AFD_POLL_WRITE` set, loops, and calls `IOCTL_AFD_POLL` again immediately, producing a 53,000 call/sec busy-spin.

---

## 3. Proposed Fix

Integrate `sock->reported_events` with `IOCTL_AFD_POLL`:
1. In `poll_flags_from_afd()`: Only query `POLLOUT` if `!(sock->reported_events & AFD_POLL_WRITE)`.
2. In `get_poll_flags()`: Only set `AFD_POLL_WRITE` if `!(sock->reported_events & AFD_POLL_WRITE)`.
3. In `complete_async_poll()`: When returning `AFD_POLL_WRITE` to the caller in `req->sockets[i].flags`, mark `sock->reported_events |= AFD_POLL_WRITE`.

When a subsequent write fails with `STATUS_DEVICE_NOT_READY`, `sock->reported_events &= ~AFD_POLL_WRITE` will clear the flag, re-enabling `POLLOUT` querying when buffer space becomes available.

---

## 4. Patch

See `patches/wine/0003-server-sock-fix-afd-poll-write-busy-spin.patch`:

```diff
diff --git a/server/sock.c b/server/sock.c
index caa32a3..70f9043 100644
--- a/server/sock.c
+++ b/server/sock.c
@@ -1121,7 +1121,7 @@ static int get_poll_flags( struct sock *sock, int event )
     }
     if (event & POLLPRI)
         flags |= is_oobinline( sock ) ? AFD_POLL_READ : AFD_POLL_OOB;
-    if (event & POLLOUT)
+    if ((event & POLLOUT) && !(sock->reported_events & AFD_POLL_WRITE))
         flags |= AFD_POLL_WRITE;
     if (sock->state == SOCK_CONNECTED)
         flags |= AFD_POLL_CONNECT;
@@ -1145,6 +1145,9 @@ static void complete_async_poll( struct poll_req *req, unsigned int status )
 
         if (sock->main_poll == req)
             sock->main_poll = NULL;
+
+        if (req->sockets[i].flags & AFD_POLL_WRITE)
+            sock->reported_events |= AFD_POLL_WRITE;
     }
 
     if (!status)
@@ -1511,7 +1514,7 @@ static int poll_flags_from_afd( struct sock *sock, int flags )
         ev |= POLLIN;
     if (flags & AFD_POLL_OOB)
         ev |= is_oobinline( sock ) ? POLLIN : POLLPRI;
-    if (flags & AFD_POLL_WRITE)
+    if ((flags & AFD_POLL_WRITE) && !(sock->reported_events & AFD_POLL_WRITE))
         ev |= POLLOUT;
 
     return ev;
```
