# CSAP Project 

## Index

1. [How to Compile](#how-to-compile)
2. [How to start the Server](#how-to-start-the-server)
3. [How to start the Client](#how-to-start-the-client)
4. [Available Commands](#available-commands)
    1. [create_user](#create_user-username-octal_perms)
    2. [login](#login-username)
    3. [create](#create-path-octal_perms)
    4. [chmod](#chmod-path-octal_perms)
    5. [move](#move-src_path-dest_path)
    6. [cd](#cd-path)
    7. [list](#list-path)
    8. [read](#read-path)
    9. [write](#write-path)
    10. [upload](#upload-client_path-server_path)
    11. [download](#download-server_path-client_path)

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
sudo ./Server /tmp/root_folder
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
- Users will be put inside the `csap-group`.
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
move test.txt test_directory/
[ERROR]: Failed to open destination file for locking (Is a directory)
```
You need to specify the already constructed path name, so:
```
move test.txt test_directory/test.txt
[Server]: 
File/Directory moved successfully
```


#### `delete <path>`
Deletes a file or a directory. Directory must be empty before deletion.
```
delete text.txt
[Server]: 
File/Directory deleted successfully 
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

#### `exit`
Closes the client.

```
exit
[CLOSE]: Exiting client.
```

