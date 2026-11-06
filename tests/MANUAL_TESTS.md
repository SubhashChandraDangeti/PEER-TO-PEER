# Manual Tests and Demo Script

These are the hands-on tests for the demo. Each one has the exact commands to type and the output you should see. Run them in order: later tests use the users, group and files created by earlier ones.

## Setup (once)

```sh
make clean && make                 # macOS first: brew install openssl@3
python3 tests/make_demo_files.py demo_files     # one file of each type
```

`tracker_info.txt` in the project root lists both trackers. Each line is `ip:client_port:sync_port`:
```
127.0.0.1:5000:6000
127.0.0.1:5001:6001
```

**Terminals.** Open 7 terminals and `cd` into the project root in each.

| Terminal | Command | Role |
|---|---|---|
| T1 | `./tracker/tracker tracker_info.txt 1` | tracker 1 |
| T2 | `./tracker/tracker tracker_info.txt 2` | tracker 2 |
| A | `mkdir -p run/a && cd run/a && ../../client/client 127.0.0.1:7001 ../../tracker_info.txt` | alice, group owner |
| B | same, with `run/b` and port `7002` | bob |
| C | same, with `run/c` and port `7003` | carol |
| D | same, with `run/d` and port `7004` | dave |
| E | same, with `run/e` and port `7005` | eve, not a member |

Each client gets its own folder so its downloads stay separate. To find a process id for the `kill` steps, use `pgrep -f "tracker_info.txt 1"`.

**Separate IP addresses (optional).** Linux treats every `127.0.0.x` address as the local machine, so on Linux you can give each client its own IP (`127.0.0.2:7001`, `127.0.0.3:7002`, ...). That way each client looks like a different machine in the logs. On macOS this needs one alias per address first, for example `sudo ifconfig lo0 alias 127.0.0.2 up`. Otherwise just use `127.0.0.1` with different ports.

---

### M1. Startup, primary election, redirect
1. Start T1, then T2 within a few seconds.
   - T1 shows `role STARTING -> PRIMARY`.
   - T2 shows `link up ... -> tracker 1 is primary`, then `role STARTING -> BACKUP`, `snapshot loaded`, and `Recovery complete: IN SYNC with tracker 1`.
2. Ask the backup directly:
   ```
   python3 tests/ask_tracker.py 127.0.0.1:5001 "LIST_GROUPS"
   ```
   It replies `NOT_PRIMARY|this tracker is the backup|127.0.0.1:5000`. Clients follow this redirect automatically.
3. In T1 type `check_sync`. You should see `IN SYNC: 0 users, ...`.

### M2. Accounts: duplicate user, uniform login error
| Terminal | Type | You should see |
|---|---|---|
| A | `create_user alice pw1` | `[OK] user alice created` |
| A | `create_user alice other` | `[ALREADY_EXISTS] user already exists` |
| A | `login nosuchuser pw1` | `[AUTH_FAILED] invalid user id or password` |
| A | `login alice wrongpw` | `[AUTH_FAILED] invalid user id or password` (the same message, so nobody can tell which usernames exist) |
| A | `login alice pw1` | `[OK] logged in as alice` |

Then create and log in bob, carol, dave and eve in B, C, D and E (`create_user bob pw1`, `login bob pw1`, and so on).

### M3. Groups and owner rules
| Terminal | Type | You should see |
|---|---|---|
| A | `create_group team` | `[OK] group team created; you are the owner` |
| B, C, D | `join_group team` | `[OK] join request sent to the owner of team` |
| A | `list_requests team` | `[OK] 3 pending request(s)` followed by bob, carol, dave |
| B | `accept_request team carol` | `[INVALID_ACTION] you are not a member of team` |
| A | `accept_request team bob` (then carol, dave) | `[OK] bob is now a member of team` |
| A | `leave_group team` | `[INVALID_ACTION] the owner cannot leave the group` |
| E | `list_groups` | `[OK] 1 group(s)` followed by `team` (any logged-in user can see groups) |

### M4. Non-member access
| Terminal | Type | You should see |
|---|---|---|
| A | `upload_file team ../../demo_files/video.mp4` | `[OK] uploaded video.mp4 to team (N pieces)` |
| E | `list_files team` | `[INVALID_ACTION] you are not a member of team` |
| E | `download_file team video.mp4 .` and then `show_downloads` | `[F] [team] video.mp4  (you are not a member of team)` |

### M5. A download is byte-for-byte identical
1. In B type `download_file team video.mp4 .`, then `show_downloads`. You should see `[C] [team] video.mp4`.
2. In a spare terminal:
   ```
   shasum demo_files/video.mp4 run/b/video.mp4      # Linux: sha1sum
   cmp demo_files/video.mp4 run/b/video.mp4         # prints nothing = identical
   ```
   Both hashes are the same. Open `run/b/video.mp4` and it plays.

### M6. Concurrent downloads
1. In A, upload `random_20mb.bin` and `tone.wav`.
2. In C type, one after another without waiting:
   ```
   download_file team random_20mb.bin .
   download_file team tone.wav .
   show_downloads
   ```
   You see `[D] ... pieces (..%)` lines, then `[C]` lines once each finishes.

### M7. Multi-seeder load balancing
A, B and C now all seed `video.mp4` (B and C became seeders automatically after their verified downloads).
1. In D type `download_file team video.mp4 .`.
2. D's log shows a line like `download team/video.mp4 complete: N pieces, per seeder: alice=.., bob=.., carol=..`.

The pieces are spread across all three seeders. Each piece goes to the seeder with the fewest requests in progress (least-connections).

### M8. The download queue
1. Run `python3 tests/make_demo_files.py demo_files --big`. This adds a 2.5 GB file.
2. In A, upload `big_2_5gb.bin`, `random_20mb.bin`, `exactly_one_piece.bin` and `notes.txt`.
3. In D, queue 4 downloads quickly, starting with the big file, then type `show_downloads`.

At most 3 downloads run at once (`[D]`). The rest wait as `[Q]` and start as slots free up.

### M9. A corrupt seeder is caught
1. Start an extra client F with the test flag. It corrupts every piece it sends:
   ```
   mkdir -p run/f && cd run/f && ../../client/client 127.0.0.1:7006 ../../tracker_info.txt --corrupt
   ```
2. In F: `create_user frank pw1`, `login frank pw1`, `join_group team`.
3. In A: `accept_request team frank`.
4. In F: `upload_file team ../../demo_files/notes.txt`. It is the same content, so frank becomes an extra seeder.
5. In D: `download_file team notes.txt ./notes_check.txt`.
6. The download completes, and `cmp` against the original prints nothing.

Every piece frank sends fails its SHA-1 check. Those pieces go back in the queue and are fetched from other seeders.

### M10. A seeder dies mid-download
1. In D start a large download (the 2.5 GB file, or `random_20mb.bin`).
2. Immediately press `Ctrl-C` in B's terminal.

The download still completes from the remaining seeders.

### M11. The primary tracker crashes
1. While a client is logged in (and ideally while it is downloading), run:
   ```
   kill -9 $(pgrep -f "tracker_info.txt 1")
   ```
2. T2 shows `primary tracker 1 is down: PROMOTING myself to primary`.
3. In A type `list_files team`. It works, with no new login: the client reconnected to T2 and resumed its session with its token.
4. Running downloads keep going, because they only talk to seeders.

### M12. Recovery: restart the failed tracker
1. Restart T1: `./tracker/tracker tracker_info.txt 1`.
2. It shows `link up ... -> tracker 2 is primary`, `role STARTING -> BACKUP`, `snapshot loaded (...)` and `Recovery complete: IN SYNC with tracker 2`.

T1 started empty and copied the whole state from T2. An empty tracker never wins the primary role.

### M13. Freeze the primary (simulated network split)
The primary is now T2.
1. Run `kill -STOP $(pgrep -f "tracker_info.txt 2")`. T2 is frozen: its sockets stay open, but it never answers.
2. After about 6 s, T1 shows `no heartbeat from tracker 2 for 6000 ms` and then `PROMOTING myself to primary`.
3. In A type `create_group splitgroup`. It waits up to 10 s, gives up on the frozen tracker, moves to T1, and replies `[OK] group splitgroup created`.
4. Run `kill -CONT $(pgrep -f "tracker_info.txt 2")`. Both trackers now think they are primary.
5. When they reconnect, both have state, so the rule says **tracker 1 wins**. T2 shows `SPLIT-BRAIN resolved: stepping down; discarding my state` and then `Recovery complete: IN SYNC`.
6. Here T1 won, and `splitgroup` was created on T1, so it survives. The losing side's changes are the ones that are lost. To see that, freeze T1 instead, so the change lands on T2, which is the tracker that later steps down.

### M14. A 2.5 GB file with flat memory
1. In D type `download_file team big_2_5gb.bin .`.
2. Watch memory in another terminal: `top -pid <client pid>` on macOS, or `top -p <pid>` on Linux. The client stays at a few MB to a few tens of MB.
3. When it finishes, compare with `shasum`.

### M15. Garbage input does not crash the tracker
```
head -c 2000 /dev/urandom | nc 127.0.0.1 5000
python3 tests/ask_tracker.py 127.0.0.1:5000 "UPLOAD_FILE|team|x|NOTNUM|zz|"
```
The second command replies `INVALID_ACTION|...`. (`NOTNUM` is the input that crashed the original tracker.) The tracker keeps running, and `check_sync` still reports IN SYNC.

### M16. Passwords are stored only as salted hashes
In the T1 console type `users`. Each line shows `alice  salt=<32 hex>  sha256=<64 hex>`. No password appears anywhere: not in this output, and not in the logs (search them for `pw1`).

### M17. `stop_share` affects only the caller (F17)
1. A and B both seed `video.mp4`.
2. In B type `stop_share team video.mp4`. You should see `[OK] stopped sharing video.mp4 in team`.
3. In C type `list_files team`. `video.mp4` is still listed. A, C and D still seed it; only bob's entry was removed.

### M18. Logout stops sharing
1. In A type `logout`. You should see `[OK] logged out alice; your files are no longer shared`.
2. In C type `list_files team`. The seeder counts drop.
3. Downloads of files only alice had now fail with `no other seeder is online`. Downloads of shared files continue from the other seeders.

### M19. Every file type downloads intact
1. In A, upload every file in `demo_files/`.
2. In D, download each one.
3. Check:
   ```
   for f in demo_files/*; do cmp "$f" "run/d/$(basename $f)" && echo "OK $f"; done
   ```
4. Open the copies to confirm:
   - the video and audio play;
   - the PNG and JPEG images open;
   - `tar xzf archive.tar.gz` extracts;
   - the executable runs after `chmod +x run/d/program.bin`. Permissions are not transferred, only the bytes.

The executable test runs only on the OS it was built for. On macOS, `program.bin` is the macOS client binary.

---

## Automated suites

| Command | What it covers |
|---|---|
| `tests/run_all.sh` | Unit, tracker, download, failover and chaos suites |
| `BIG=1 tests/run_all.sh` | Also the 2.5 GB file |
| `LEAKS=1 tests/run_all.sh` | Also valgrind, where it is installed |
