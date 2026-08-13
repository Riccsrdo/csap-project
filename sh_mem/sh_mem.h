#ifndef SH_MEM_H
#define SH_MEM_H

#include <sys/types.h>
#include <sys/ipc.h>
#include<sys/shm.h>
#include<limits.h>
#include<stdio.h>
#include<errno.h>
#include<sys/sem.h>
#include<string.h>

#define MAX_ENTRIES 1024

/*
enum to hold the status of a transfer request:
- Pending: the request is waiting for a response
- Accepted: the request has been accepted and the transfer can proceed
- Rejected: the request has been rejected and the transfer cannot proceed
*/
enum request_status {
    PENDING = 0,
    ACCEPTED = 1,
    REJECTED = 2
};

union semun { // union used for semctl() to set the value of the semaphore
        int val; // value for SETVAL
        struct semid_ds *buf; // buffer for IPC_STAT and IPC_SET
        unsigned short *array; // array for GETALL and SETALL
};


typedef struct {
    char username[33]; // 32 characters
    pid_t pid; // process ID
    int in_use; // flag to indicate if the entry is in use, with 1 being in use and 0 being free
} session_entry_t;

// array of session_entry_t to hold the entries
typedef struct {
    session_entry_t entries[MAX_ENTRIES];
    int count; // number of entries currently in use
} session_table_t;

typedef struct {
    int id; // unique identifier for the request
    char source_username[33]; // username of the source process
    char dest_username[33]; // username of the destination process
    char file_path_absolute[PATH_MAX]; // path to the file being requested
    enum request_status status; // status of the request
} transfer_request_entry_t;

typedef struct {
    transfer_request_entry_t entries[MAX_ENTRIES];
    int count; // number of pending requests
    int next_id; // next unique identifier to be assigned to a new request
} pending_requests_table_t;

// master structure used by shmget
typedef struct {
    session_table_t session_table;
    pending_requests_table_t pending_requests_table;
} shared_memory_t;

int setup_shmem(int *smh_id, int *sem_id, shared_memory_t **shm_ptr_out, gid_t owner_gid);

int sem_lock(int sem_id);
int sem_unlock(int sem_id);

void cleanup_shmem(int shm_id, int sem_id, shared_memory_t *shm_ptr);

#endif