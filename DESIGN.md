## Architectural choices
- For TCP, we define a specific frame format, to distinguish the different messages a server might receive. A format could be given by | preamble (x2 Byte) | command (x1 Byte)| len of payload (x4 Byte) | payload (xLen Bytes) |. Similarly, for the answer | preamble (x2 Byte) | status (x1 Byte, OK, ERR)| len of payload (x4 Byte) | payload (xLen Bytes, either payload for OK, or err_code+ payload)|
- For each connection, we fork() a child, managing that user.

## Server

- I use select() call to detect if either the user launching the server is writing something on CL, or if the client woke up and is writing something, using a set of read file descriptors that is reset at every iteration, adding the stdin and the file descriptor of the remote client. I set up the timeout to be NULL to have infinite blocking until activity is detected.


## Commands

### List
For list, to capture stats about files, I decided to use `lstat` instead of the `stat` function, as the latter resolves the link to the linked file, potentially allowing users to escape the sandboxed environment, while `lstat` returns metadata of the actual soft link.

