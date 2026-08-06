## Architectural choices
- For TCP, we define a specific frame format, to distinguish the different messages a server might receive. A format could be given by | preamble (x2 Byte) | command (x1 Byte)| len of payload (x4 Byte) | payload (xLen Bytes) |. Similarly, for the answer | preamble (x2 Byte) | status (x1 Byte, OK, ERR)| len of payload (x4 Byte) | payload (xLen Bytes, either payload for OK, or err_code+ payload)|
- For each connection, we fork() a child, managing that user.
- For the send() and recv() operations I implemented a mechanism to handle errors like EINTR, which means the operation has been interrupted (maybe due to the fact a SIGCHLD was received during the receival of bytes).
- I use htons() and htonl() functions for writing to handle endianess, as in normal configurations we may have machines communicating, using different endianess mechanisms. By enforcing the use of these functions for integer values, alongside ntohs and ntohl in reading, I guarantee the correct order in interpreting the value.
- I implemented a chunking mechanism to send data and receive large portions of data. Read, write, upload and download operations have a command to start sending/receiving data, and one to indicate the end of the process. Given CHUNK_SIZE, I send/receive iterating until the expected amount has finished. 
- When uploading a file, to avoid overwriting it if the operations has not been completed successfully, I open a temporary file, and only when the server receives END it overwrites the content. Similarly for writing, but only when the offset has not been specified, as I want to avoid truncating the file until all the data has been received.
- For the locking mechanism, I decided to use non-blocking locks, as I don't want other clients to remain hanging while waiting. I simply notify them about the inability of locking a file, at that time.

## Server

- I use select() call to detect if either the user launching the server is writing something on CL, or if the client woke up and is writing something, using a set of read file descriptors that is reset at every iteration, adding the stdin and the file descriptor of the remote client. I set up the timeout to be NULL to have infinite blocking until activity is detected.
- As for the number of currently connected clients, I used a volatile value, due to the fact I setup a signal handler to handle SIGCHILD. The receival of a SIGCHILD indicates that a client has been disconnected, therefore we need to decrement the value, and because it's decremented within a signal handler, we use the volatile definition. Source I found online: https://stackoverflow.com/questions/246127/why-is-volatile-needed-in-c
- for handling notifications between different children (used in transfers), I use a named pipe, the FIFO, with each child having its own FIFO in which processes can write, stored in .sessions/, and the name is determined by the PID.
- I start the server with root privileges (using `sudo`). After performing a login through the child process, the privileges are dropped to user's ones (if the user has not been created yet, I create it), and to common group id, through `seteuid()` and `setegid()`. We can perform an escalation of privileges as the saved-set-uid is 0 (root), if necessary.
- I configure `umask(0)` to ensure the creation of files and directories with the specified permissions, otherwise requested permissions are && with the umask, and correct permissions might not be granted.
- for each received command, I follow the same idea: first I validate the provided path (if available), then I take a lock on resources (if necessary), I call the function in fsops (file), answer based on the returned value, and release resources.

## Client

- I use the forking mechanism for handling background operations as well, and then use a pipe to communicate with the father process about the fact an operation has been completed. Furthermore, for each background operation, I open a direct connection to the server, removing the need of having to manage packets related to two different operations.

## Commands

### CD
I use chdir() when performing a cd operation, as the kernel maintains the current working directory.

### List
For list, to capture stats about files, I decided to use `lstat` instead of the `stat` function, as the latter resolves the link to the linked file, potentially allowing users to escape the sandboxed environment, while `lstat` returns metadata of the actual soft link.

### Delete
For the locking mechanism, I decided to apply the lock to the parent directory. When I call `unlink()` on the file, the entry is removed from the directory in which the file is contained. If another client tries, in the meanwhile, to create a file with the same name, he will be able to do so.
Both the client deleting the file, and the client opening the file, will believe they have full control over the file, while in reality they're working with two different inodes (race condition). By locking parent directory, I stop every other client trying to modify of create the file to do so, while the other client is performing the `unlink()`.

### Move
For the locking mechanism, to avoid a deadlock due to the lock of the source dir and destination dir, I decided to:
- Obtain a write lock on the source file, ensuring no one is performing operations on it.
- If the destination file already exists, I acquire a lock on that as well, avoiding the fact other clients might be perfoming operations on it.