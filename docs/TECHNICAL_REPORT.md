# Technical Report: Fault-Tolerant P2P File Sharing

## 1. Approach

I first audited the original assignment submission and reproduced its failures before writing any new code. Four tests against the original tracker showed the problems:

1. A non-numeric file size crashed the whole tracker with an uncaught `std::stol` exception.
2. Any process could connect to the sync port and create accounts on the tracker.
3. Any network client could shut the tracker down with `QUIT`.
4. A non-member could read a group's files and register a fake seeder under another user's name.

The code review also found these problems:

- **Files over about 100 MB failed.** The tracker's hash list (40 hex characters per piece) was read with a single 8 KB `recv`, so it was cut off at about 199 pieces.
- **Every downloaded piece was held in RAM.** A 1 GB file needed 1 GB of memory.
- **Data races.**
  - Several worker threads wrote to the same `vector<bool>` with no lock, and `vector<bool>` packs many flags into each byte, so writes interfered.
  - The tracker shared a `static` map across all client threads without a lock.
- **Downloads blocked the command line**, so a client could not run two downloads at once.
- **Tracker sync could not handle deletions.** A full sync only added items, so removed items came back. A group dissolve was never synced at all.

I then wrote down requirements: every assignment feature, plus the fault-tolerance and authentication features I chose to add, each with a reason. The code was rebuilt in six phases. Each phase had automated exit tests, and the next phase did not start until they passed.

| Phase | Content | Exit test |
|---|---|---|
| 0–1 | Module layout, Makefiles, framing, validation, hashing | 62 unit checks |
| 2 | Tracker commands, authentication, permissions | 65 checks, including fuzzing |
| 3 | Client, peer server, download engine | 99 checks, plus a 2.5 GB test |
| 4 | Replication and failover | 28 checks |
| 5 | Hardening | Chaos test (44 checks), valgrind, ThreadSanitizer, 10 clean full runs |
| 6 | Documentation | Numbers checked against test output |

## 2. Synchronization algorithm

**Choice: primary/backup with synchronous replication.**

The spec requires exactly two trackers and continued operation while at least one is up. That rules out majority-based consensus (Raft, Paxos): with two nodes, a majority is both nodes, so the system would stop whenever one failed. Of the remaining options:

| Option | Why it was rejected or chosen |
|---|---|
| Both trackers accept writes (multi-master) | Concurrent writes can conflict. That needs conflict resolution (Lamport clocks, tie-breaking rules) and tombstones for deletions. All of that is extra machinery to explain and debug. |
| Primary/backup | One writer orders every change, so there are no conflicts during normal operation. **Chosen.** |

**The op model.** Every change is a text op, applied by one deterministic function, `State::apply`. This gives three things for free:

- **Replication** is sending the op to the backup.
- **A snapshot** is the list of ops that rebuilds the current state.
- **`check_sync`** compares hashes of that list.

Randomness (salts, tokens) is generated before the op is built and carried inside it, so the primary and backup can never diverge because each generated different random values.

**Synchronous replication.** The primary holds its single state mutex for the whole sequence:

1. Validate the request.
2. Send the op to the backup.
3. Wait for the backup's ACK (giving up after 6 s).
4. Apply the op locally.
5. Reply to the client.

While replication succeeds, the backup applies changes in the primary's order before the reply. If replication fails, the primary continues alone; those degraded writes can be lost if that primary subsequently fails. The cost is one round trip on the local network per write, and requests are serialized. At this system's scale (a few clients, small operations) that cost is negligible.

**Failure detection and promotion.** The trackers exchange a heartbeat every 2 s. A closed link, or 6 s of silence, means the partner is down, and the backup promotes itself. Silence matters because a frozen process (`kill -STOP`) keeps its TCP connection open, so the kernel never reports the link as closed.

**Recovery.** A restarted tracker always starts empty, joins as backup and loads a snapshot. The primary holds its mutex while sending the snapshot and attaches the backup in the same critical section, so no change can fall between the snapshot and the first replicated op. The recovered tracker then runs `check_sync` automatically.

**Split-brain.** With two nodes, a crashed partner and an unreachable one look identical, so the backup must promote itself to stay available. Both trackers may then be primary at once. When they reconnect, the HELLO rule picks the winner:

- a tracker with empty state never wins;
- otherwise, tracker 1 wins.

The loser recovers exactly like a restarted tracker. The cost is that changes the loser accepted during the split are lost. That is the price of availability with two nodes; avoiding it needs a third node to break the tie.

## 3. Piece selection strategy

**Sequential order through a shared work queue.** Each worker pops the next piece number, so a piece has one owner and is never downloaded twice.

**Seeder choice is least-connections.** Each request goes to the seeder with the fewest requests in flight, the standard rule load balancers use. Ties go to the seeder that has served the fewest pieces so far. Without that tie-break, the first seeder in the list won every tie, and the split was 101/60/39; with it, the split was 73/64/63.

Rarest-first, BitTorrent's strategy, was rejected. In this system every seeder holds the complete file, because only uploaders and downloaders who have verified the whole file register as seeders. Every piece is equally available, so "rarest" means nothing.

**Failure handling.** A failed or corrupt piece goes back on the queue, and the seeder that failed it is skipped for that piece. After 6 attempts the download is marked `[F]` and its `.part` file is deleted. This bounded retry replaced the original infinite retry loop.

## 4. Protocol design rationale

**Framing.** Every message carries a 4-byte length and a 1-byte type. TCP is a byte stream with no message boundaries, and that was the root cause of the original 100 MB limit. A length prefix (rather than a newline delimiter) was chosen because piece data is binary and can contain any byte, including a newline.

**Bounded allocation.** The length is checked against 1 MiB before any memory is allocated, so a corrupt or malicious length cannot force a huge allocation.

**Text bodies.** Control messages use text fields separated by `|`. They are easy to read in logs and to debug. Name validation guarantees no field can contain `|`.

**Seven reply codes.** Each code tells the client what to do next:

| Code | What the client does |
|---|---|
| `NOT_PRIMARY` | Follow the redirect address in the reply |
| `NOT_LOGGED_IN` | Log in |
| `AUTH_FAILED` | Fix the credentials |
| `INVALID_ACTION`, `NOT_FOUND`, `ALREADY_EXISTS` | Show the error to the user |

Unknown user and wrong password get the same `AUTH_FAILED` reply, so the reply does not reveal which usernames exist.

## 5. Challenges and solutions

| Challenge | Solution |
|---|---|
| Files over ~100 MB failed; 1 GB used 1 GB of RAM | Length-prefixed frames. Pieces written with `pwrite` straight into a `.part` file. 64-bit offsets. A 2.5 GB file now uses 14 MB at peak. |
| One malformed number killed the tracker | Hand-written `parse_u64` (digits only, overflow-checked, never throws). Every field validated. 300-message fuzz test. |
| Downloads blocked the CLI | 3 download slot threads and a FIFO queue, with 4 workers per download, so thread count is fixed. |
| **Bug found by the freeze test:** a client kept retrying the frozen tracker first, waiting 10 s each time | The tracker that just failed is moved to the end of the connection order. |
| **Bug found by the freeze test:** when the frozen tracker resumed, it handled the client's abandoned connections, saw them close, and treated that as a logout, ending a session that was live on the other tracker | A disconnect now only stops sharing (an op that removes the user's seeder entries). A session ends only on `logout` or when a newer login replaces it. Clients resume with their token. |
| **Bug found by ThreadSanitizer:** heartbeat and replication threads copied the link's file descriptor, then sent on it without a lock, while another thread could close it. The OS can reuse a closed descriptor number, so a heartbeat could be sent on an unrelated socket. | One mutex now guards the descriptor and every send on it, and the descriptor is closed while holding that mutex. |
| **Possible deadlock found in design review:** answering `check_sync` needs the state mutex. On the primary, that mutex can be held by a request waiting for an ACK, which the link-reader thread has not read yet. | `CHECK_REQ` is answered by a separate responder thread, so the link reader never waits on the state mutex. |
| The tracker never cleaned up finished per-client threads; each one keeps its stack until joined | Finished threads are joined whenever a new connection is accepted. The chaos test confirms thread and file-descriptor counts stay flat across 20 failovers. |
| `close()` does not wake a blocked `accept()` on macOS | Accept loops use `poll` with a 200 ms timeout and check a stop flag. |
| Least-connections always broke ties toward the first seeder | Ties now go to the seeder that has served the fewest pieces. |

## 6. References

- B. Cohen, *Incentives Build Robustness in BitTorrent*, 2003. Background on piece selection and why rarest-first matters when seeders hold different pieces.
- AOS Assignment 3 specification, IIIT Hyderabad, Monsoon 2025.
- OWASP Password Storage Cheat Sheet. Salting, and why slow hashes (PBKDF2, bcrypt, Argon2) are preferred; this system's salted SHA-256 is a documented limitation.
- Linux man pages: `socket(7)`, `tcp(7)`, `pread(2)`, `pwrite(2)`, `open(2)` (`O_NOFOLLOW`), `rename(2)` (atomic replace), `poll(2)`.
- OpenSSL 3 documentation: `EVP_Digest*`, `RAND_bytes`, `CRYPTO_memcmp`.
- The primary/backup and split-brain trade-offs follow standard material on replication (CAP theorem; why ZooKeeper and etcd use 3 or 5 nodes).
