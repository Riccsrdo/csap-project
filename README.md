# CSAP Project 

## Index

1. [How to Compile](#how-to-compile)
2. [How to start the Server](#how-to-start-the-server)
3. [How to start the Client](#how-to-start-the-client)
4. [Available Commands](#available-commands)
    - [Before Login](#before-login)
        1. [create_user](#create_user-username-octal_perms)
        2. [login](#login-username)
    - [After Login](#after-login)
        1. [create](#create-path-octal_perms)
        2. [chmod](#chmod-path-octal_perms)
        3. [move](#move-src_path-dest_path)
        4. [cd](#cd-path)
        5. [list](#list-path)
        6. [read](#read-path)
        7. [write](#write-path)
        8. [upload](#upload-client_path-server_path)
        9. [download](#download-server_path-client_path)
        10. [delete](#delete-path)
        11. [exit](#exit)
    - [Transfer Request](#transfer-request-mechanism)
        1. [transfer_request](#transfer_request-file-dest_user)
        2. [accept](#accept-directory-id)
        3. [reject](#reject-id)
5. [Design choices](#design-choices)
6. [Project Structure](#project-structure)
    

## How to Compile
Run the following, inside the project's folder:
```bash
make
```

Two files are outputted: **Server** and **Client**.

To clean, run:
```bash
make clean
```

Through the `Makefile`, the project is compiled using `gnu11` and `-Wall -Wextra`, showing all warnings.
All subfolders are included as well.

## How to start the Server
```bash
sudo ./Server <root_dir> [<IP>] [<port>]
```

| Parameter | Mandatory? | Default value | Info |
| --- | --- | ---| --- |
| `<root_dir>` | yes | - | Specifies the root directory in which virtual file systems are created |
| `<IP>`| no | '127.0.0.1' | Listening address |
| `<port>` | no | '8080' | Listening port |

**Example**:
```bash
sudo ./Server /tmp/root2
```

The server uses `sudo` to create real system users through `adduser`, as well as creating the common group at first start with `groupadd`, and to assign home directory properties with `chown`. Furthermore, it's temporarily used to perform the copying operation in the `transfer_req` module.
`sudo` privileges are dropped as soon as possible, specifically when a children for the connected client is spawned, and there permissions of the previously created user are applied through `seteuid()` and `setegid()`.

### Initial Output
```
[SETUP]: Root directory created and set to /tmp/root2
[SETUP]: Resolved root directory path: /tmp/root2
[SETUP]: .sessions directory created at /tmp/root2/.sessions
[SETUP]: Server group ID initialized to 1001
[SETUP]: Socket created successfully.
[SETUP]: Socket bound to 127.0.0.1:8080
[SETUP]: Listening for incoming connections...
[SETUP]: Server started on 127.0.0.1:8080
[SETUP]: Server loop started. 	Type 'exit' to close
```

### Terminating the server
Either by typing `exit` on the terminal, or send a signal like `SIGINT`.

## How to start the Client
```bash
./Client [<IP>] [<port>]
```

| Parameter | Mandatory? | Default value | Info |
| --- | --- | ---| --- |
| `<IP>` | no | '127.0.0.1' | Listening address |
| `<port>` | no | '8080' | Listening port |

No privileges required in particular.
Output:
```
[SETUP]: Socket created for client
[SETUP]: Client started. Connected to server at 127.0.0.1:8080
```

From now on, it's possible to send commands through the terminal, pressing `ENTER` after typing them.

Output types are the following:
| Initial part | Meaning |
| --- | --- |
| `[Server:]` | Positive response from the server |
| `[ERROR]:` | Error message, due to some problems occurred in the server |
| `[INFO]:` | Informative message coming from the client |
| `[NOTIFY]:` | Used for `transfer_req` |
| `[BACKGROUND]` | Used to confirm the completion of a background operation |

To terminate the client, type:
```bash
exit
```
If there are any background processes working, termination will be refused.
```
[INFO]: There are 1 background operations running. Exiting is not permitted until they are completed.
```

## Available commands
### Before login
#### `create_user <username> <octal_perms>`

Creates a new user and its home dir inside the server's root.
Rules:
- Username must be 3 to 32 characters long, following standard naming conventions for usernames in UNIX like systems;
- Permissions are in octal, following `0000` to `0777`.
- Users will be put inside the `csap_group`.
- No password required.

```bash
create_user marco 0750
[SERVER]: OK
```
IMPORTANT: To allow `list` to operate, use correct permissions for the `group` byte.

#### `login <username>`
Authenticate the session, establishing whos sending commands. 
Provides access to home dir of the user.

Possible errors: Non-existing user, Missing home directory.

```
login marco
[Server]: 
Logged in as marco
```

### After login

#### `create <path> <octal_perms>`
Possible options:
- `-d`: create a directory.

```
create test.txt 0750
[Server]: 
File/Directory created successfully 
create test_directory -d 0750
[Server]: 
File/Directory created successfully
```

Fails if the file already exists. Creation uses `O_EXCL`, so never overwrites.

#### `chmod <path> <octal_perms>`
Applies required octal permissions to file (`umask` is set to 0 at server's setup to guarantee correc appliance).
```
chmod test.txt 0740 
[Server]: 
Permissions changed successfully
```

#### `move <src_path> <dest_path>`
Moves by renaming a file or directory to required path (needs to be inside user's home).

```
move test.txt test_directory/test.txt
[Server]: 
File/Directory moved successfully
```


#### `cd <path>`
Change current working directory
```
cd test_directory
[Server]: 
Directory changed successfully
```

#### `list <path>`
Shows the content of a directory (or info of a file), emulating `ls -l`.
If no `<path>` is provided, shows content of CWD.

```
list
drwxr-x--- marco csap_group 2 4096 Sun Aug 16 22:12:11 2026 test_directory
-rwx-wx--- marco csap_group 1 0 Sun Aug 16 22:13:27 2026 test_file.md
[Server]: 145 bytes listed
list .
drwxr-x--- marco csap_group 2 4096 Sun Aug 16 22:12:11 2026 test_directory
-rwx-wx--- marco csap_group 1 0 Sun Aug 16 22:13:27 2026 test_file.md
[Server]: 145 bytes listed
list ..
drwxrwx--- root csap_group 2 4096 Sun Aug 16 22:08:46 2026 .sessions
drwxr-x--- marco csap_group 3 4096 Sun Aug 16 22:13:27 2026 marco
[Server]: 135 bytes listed
list test_directory/
-rwxr----- marco csap_group 1 0 Sun Aug 16 22:10:11 2026 test.txt
[Server]: 66 bytes listed

```
Allows to leave the scope of user's home dir up to root dir.

#### `read <path>` 
Read on `stdout` of the client the content of specified file in path.
Options:
- `-offset=N`: sending starts from byte `N`.

```
read test_file.md
Hello!!! :3
[Server]: 12 bytes received
```
With `-offset=N`:
```
read -offset=10 test_file.md
4
[Server]: 2 bytes received
```

#### `write <path>`
Writes in specified file using content written in `stdin` of the client. To send, use `CTRL+D` on empty line.
Options:
- `-offset=N`: Writing takes place starting from `N` bytes in dest. file.

```
write test_file.md
[INFO]: type the content, then press Ctrl-D to end the input
Hello!!! :3
[Server]: 12 bytes written
```

With `-offset=N`:
```
write -offset=10 test_file.md
[INFO]: type the content, then press Ctrl-D to end the input
4
[Server]: 2 bytes written
```

#### `upload <client_path> <server_path>`
Sends a local file to server.
Options:
- `-b`: Executes the operation in background, spawning a child in the client, handling the connection and transferring.

```
upload /tmp/new_file.txt new_file.txt
[Server]: 29 bytes written

upload -b /tmp/new_file2.txt new_file2.txt
[INFO]: Background operation started. Total background operations: 1
[Server]: 29 bytes written
[Background] Command: upload new_file2.txt /tmp/new_file2.txt concluded
```

#### `download <server_path> <client_path>`
Downloads a file from server to client-
Options:
- `-b`: similar mechanism to upload.

```
download new_file.txt /tmp/test_dir/new_file.txt
[Server]: 29 bytes received
download new_file.txt /tmp/test_dir/new_file.txt -b
[INFO]: Background operation started. Total background operations: 1
[Server]: 29 bytes received
[Background] Command: download new_file.txt /tmp/test_dir/new_file.txt concluded
```

#### `delete <path>`
Deletes a file or a directory. Directory must be empty before deletion.
```
delete text.txt
[Server]: 
File/Directory deleted successfully 
```

#### `exit`
Closes the client.

```
exit
[CLOSE]: Exiting client.
```

### Transfer Request Mechanism
Allows one user to copy a file in his virtual file system into another user's file system, having the destination user accept the request.

#### `transfer_request <file> <dest_user>`
First, user performs request, waiting for the other user to accept:
`marco`:

```
transfer_request temp.txt raffaele
```

The other user receives the request:
`raffaele`:

```
[NOTIFY]: Transfer request with ID 1 received for file 'temp.txt'
```

#### `accept <directory> <ID>`
Destination user accepts the request, obtaining the ID from the previously printed message on `stdout`.
```
accept . 1
[Server]:
Transfer request accepted and file copied to temp.txt
```

Source user receives:
```
[Server]:
Transfer request with ID 1 accepted
```

#### `reject <ID>`
Destination user rejects the request:
```
reject 2
[Server]:
Transfer request rejected successfully
```

And source user:
```
[ERROR]: Transfer request with ID 2 rejected (Operation canceled)
```

#### Important 
- IDs are incremental;
- Only one destination user can accept or reject the request;
- Transfer from multiple source user is permitted;
- If the source user closes the connection while the request is pending, the request is removed.

## Design Choices

### Transfering does not overwrite files
If the destination directory already contains a file with the same name, copying will fail with `File exists`. 

### Symbolic Links are not followed in transfers and list
To stop attackers from abusing root privileges of the Server to read content outside the allowed scope of the root folder.

### `upload` and `write` are atomical
Content is first written in a temporary file in the same directory as the destination, and then `rename()`-d. If the connection drops, the old file is not modified. 
Exception is given by `write -offset=N`, which overwrites a portion of the file, thus operating on it directly.

### Locks are non-blocking
If another client is operating on a file, instead of letting the user's wait without any explanation, they receive a `Resource temporarily unavailable`. 

Here is the matrix for lock operations:
| Operation | Required Lock | Info |
| --- | --- | ---| 
| `read`, `download` | read lock | simultaneous readings allowed |
| `write`, `upload` | write lock | no other read or write ops. |
| `delete`| write lock | like write |
| `move` | write lock on source (and destination if exists)| same as above |

### Sandbox
Each user is confined within its home. Each path is subject to a canolization first with `realpath()` function, and then checked with root folder.
Nothing is allowed to leave user's home except `list`.

### Termination
- Termination of client is handled taking the number of background operations into consideration, waiting for all of them to finish.
- All child processes are collected through a `SIGCHLD` handler.
- All shared memory segments are correctly removed at closure.


## Project Structure
```
.
|
|- makefile
|
|- main-server.c        start, parsing of argv, loop accept + fork, commands dispatch
|
|- main-client.c        loop on stdin/socket/pipe, background ops
|
|- network/             framing of packets, send/recv of bytes with EINTR management, send_ok/send_err from server to client
|
|- protocol/            commands parsing and options
|
|- fsops/               create, chmod, move, delete, cd, list
|
|- paths/               path canocalization and sandbox validation
| 
|- session/             user creation, login, privilege management
|
|- transfer/            upload, download, copy for transfer_req
|
|- locks/               wrapper for fcnlt for record locks
|
|- sh_mem/              System V shared memory and semaphores handling
|
|- utils/               dynamical buffers, write_all to ensure correct writing, read_line to read until '\n', permission parsing
```

#### Network Protocol
```
| preamble (2 B) | command/answer state (1 B) | payload lenght (4 B) | payload (N B) |
```
Up to 64 KB packets.

See `DESIGN.md` for more informations in depth.

