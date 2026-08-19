#include"sh_mem.h"

int sem_lock(int sem_id){
    struct sembuf sb;
    sb.sem_num = 0; // semaphore index
    sb.sem_op = -1; // decrement operation (lock)
    sb.sem_flg = SEM_UNDO; // automatically undo the operation if the process exits

    while(semop(sem_id, &sb, 1) < 0){
        if(errno == EINTR) continue; // interrupted by signal, retry
        return -errno; 
    }
    return 0; 
}

int sem_unlock(int sem_id){
    struct sembuf sb;
    sb.sem_num = 0; // semaphore index
    sb.sem_op = 1; // increment operation (unlock)
    sb.sem_flg = SEM_UNDO; // automatically undo the operation if the process exits

    while(semop(sem_id, &sb, 1) < 0){
        if(errno == EINTR) continue; // interrupted by signal, retry
        return -errno; 
    }
    return 0; 
}



int setup_shmem(int *smh_id, int *sem_id, shared_memory_t **shm_ptr_out, gid_t owner_gid){
    // I used 0600 permissions to ensure only the owner can read/write (the father)
    int shm_id_temp = shmget(IPC_PRIVATE, sizeof(shared_memory_t), IPC_CREAT  | 0600);
    if(shm_id_temp < 0){
        return -errno;
    }

    *smh_id = shm_id_temp;

    // attach to the shared memory segment
    // NULL means the system chooses the address at which to attach the segment
    shared_memory_t *shm_ptr = (shared_memory_t *)shmat(shm_id_temp, NULL, 0);
    if(shm_ptr == (void *)-1){
        int saved = errno;
        shmctl(shm_id_temp, IPC_RMID, NULL); // remove the shared memory
        *smh_id = -1;
        return -saved;
    }

    memset(shm_ptr, 0, sizeof(shared_memory_t)); // initialize the shared memory to zero
    // set first id of the pending requests table to 1
    shm_ptr->pending_requests_table.next_id = 1;

    // I use 0660 to ensure also children belonging in the "csap_group" can modify the semaphore
    int sem_id_temp = semget(IPC_PRIVATE, 1, IPC_CREAT | 0660);
    if(sem_id_temp < 0){
        int saved = errno;
        shmdt(shm_ptr); 
        shmctl(shm_id_temp, IPC_RMID, NULL); 
        *sem_id = -1;
        return -saved;
    }

    *sem_id = sem_id_temp;

    // initialize the union for semctl
    union semun arg;

    struct semid_ds sem_info;
    arg.buf = &sem_info;


    if(semctl(sem_id_temp, 0, IPC_STAT, arg) == 0){
        sem_info.sem_perm.gid = owner_gid; // set the group ID of the semaphore to the server's group ID, otherwise it's created with root group
        sem_info.sem_perm.mode = 0660; // set the permissions of the semaphore to 0660
        if(semctl(sem_id_temp, 0, IPC_SET, arg) < 0){
            int saved = errno;
            shmdt(shm_ptr); // detach from the shared memory
            shmctl(shm_id_temp, IPC_RMID, NULL); 
            semctl(sem_id_temp, 0, IPC_RMID); // remove the semaphore
            *sem_id = -1;
            *smh_id = -1;
            return -saved;
        }
    }

    union semun arg2;
    arg2.val = 1; // initialize the semaphore to 1 (unlocked)

    if(semctl(sem_id_temp, 0, SETVAL, arg2) < 0){
        int saved = errno;
        shmdt(shm_ptr); 
        shmctl(shm_id_temp, IPC_RMID, NULL);
        semctl(sem_id_temp, 0, IPC_RMID); 
        *sem_id = -1;
        *smh_id = -1;
        return -saved;
    }

    *shm_ptr_out = shm_ptr;
    *smh_id = shm_id_temp;
    *sem_id = sem_id_temp;

    // mark the shared memory for deletion, it will be deleted when all processes detach
    shmctl(shm_id_temp, IPC_RMID, NULL);
    
    return 0; 
}

void cleanup_shmem(int shm_id, int sem_id, shared_memory_t *shm_ptr){
    if(shm_ptr != NULL){
        shmdt(shm_ptr); 
    }
    if(shm_id >= 0){
        shmctl(shm_id, IPC_RMID, NULL); 
    }
    if(sem_id >= 0){
        semctl(sem_id, 0, IPC_RMID); 
    }
}

/*
Functions used for direct operations on shared memory, without requiring calling process to use sem_lock and sem_unlock.
*/

/*
Records user session with request pid, using first available entry in the session table.
If no entry is available, returns -ENOMEM.
*/
int shm_session_add(shared_memory_t *mem, int sem, const char *user, pid_t pid){
    int r = sem_lock(sem);
    if(r < 0){
        return r;
    }

    // first create a local session_entry_t with the given user and pid
    session_entry_t entry = {0};
    entry.pid = pid;
    entry.in_use = 1;
    snprintf(entry.username, sizeof(entry.username), "%s", user);

    int rc = -ENOMEM; // default to no available entry

    // iterate through the session table to find an available entry
    for(int i = 0; i < mem->session_table.count; i++){
        if(mem->session_table.entries[i].in_use == 0){
            mem->session_table.entries[i] = entry; // copy the entry into the available slot
            rc = 0;
            break;
        }
    }

    if(rc != 0 && mem->session_table.count < MAX_ENTRIES){ // no entry was found, but there is still space in the table
        mem->session_table.entries[mem->session_table.count] = entry;
        mem->session_table.count++;
        rc = 0;
    }
    
    sem_unlock(sem);
    return rc;
}

/*
Look for requested user session in the table.
If found, returns 1. If not found, returns 0. If an error occurs, returns a negative value.
Pid and online are populated.
*/
int shm_session_find(shared_memory_t *mem, int sem, const char *user, pid_t *pid, int *online){
    int r = sem_lock(sem);
    if(r < 0){
        return r;
    }

    int found = 0; // bool to indicate if the user was found

    // iterate through the session table to find the requested user
    for(int i = 0; i < mem->session_table.count; i++){
        session_entry_t *entry = &mem->session_table.entries[i];

        // match on username is found
        if(strcmp(entry->username, user) == 0){
            found = 1;
            if(pid != NULL){
                *pid = entry->pid;
            }
            if(online != NULL){
                *online = entry->in_use;
            }
            break;
        }

    }

    sem_unlock(sem);
    return found;
}

/*
Remove from sessions table all entries for not existing processes, by checking if the pid is still alive.
Returns 0 or -erno.
*/
int shm_sessions_collect_stale(shared_memory_t *mem, int sem){
    int r = sem_lock(sem);
    if(r < 0){
        return r;
    }

    for(int i = 0; i < mem->session_table.count; i++){
        if(!mem->session_table.entries[i].in_use){
            continue; // entry in use
        }

        if(kill(mem->session_table.entries[i].pid, 0) < 0 && errno == ESRCH){ // send a 0 signal to check if the process exists, if it returns -1 and errno is ESRCH, the process does not exist
            memset(&mem->session_table.entries[i], 0, sizeof(session_entry_t)); // clear the entry
            mem->session_table.entries[i].pid = -1;
        }
    }

    sem_unlock(sem);
    return 0;
}

/*
Insert a new request in PENDING state.
Returns assigned ID or -errno.
*/
int shm_request_add(shared_memory_t *mem, int sem, const char *src_user, const char *dest_user, const char *abs_path){

    int r = sem_lock(sem);
    if(r < 0){
        return r;
    }

    transfer_request_entry_t entry = {0};
    snprintf(entry.source_username, sizeof(entry.source_username), "%s", src_user);
    snprintf(entry.dest_username, sizeof(entry.dest_username), "%s", dest_user);
    snprintf(entry.file_path_absolute, sizeof(entry.file_path_absolute), "%s", abs_path);
    entry.status = PENDING;

    // look for an available index
    int idx = -1;
    for(int i = 0; i < mem->pending_requests_table.count; i++){
        if(mem->pending_requests_table.entries[i].status != PENDING){ // if the entry is not pending, it can be reused
            idx = i;
            break;
        }    
    }

    if(idx < 0){ // no availble index, check if new index can be used
        if(mem->pending_requests_table.count < MAX_ENTRIES){
            idx = mem->pending_requests_table.count;
            mem->pending_requests_table.count++;
        } else {
            sem_unlock(sem);
            return -ENOMEM; 
        }
    }

    int id = mem->pending_requests_table.next_id++;
    entry.id = id;
    mem->pending_requests_table.entries[idx] = entry;

    sem_unlock(sem);
    return id;
}

/*
Handles a request by ID, changing the status to new_status. 
Returns 0, or -ENOENT if the request is not found, or -EACCES if the request is not for the given dest_user, or -errno.
*/
int shm_request_take(shared_memory_t *mem, int sem, int id, const char *dest_user, enum request_status new_status, transfer_request_entry_t *out) {

    int r = sem_lock(sem);
    if(r < 0){
        return r;
    }

    int rc = -ENOENT;

    for(int i = 0; i < mem->pending_requests_table.count; i++){
        transfer_request_entry_t *entry = &mem->pending_requests_table.entries[i];

        if(entry->id == id){
            if(strcmp(entry->dest_username, dest_user) != 0){
                rc = -EACCES; // request is not for the given dest_user
                break;
            }

            if(entry->status != PENDING){
                rc = -EALREADY; // request already handled
                break;
            }

            entry->status = new_status;
            if(out != NULL){
                *out = *entry;
            }
            rc = 0;
            break;
        }
    }

    sem_unlock(sem);
    return rc;
}

/*
Used to force the state to st when copy fails during a fail.
*/
int shm_request_set(shared_memory_t *mem, int sem, int id, enum request_status st){
    int r= sem_lock(sem);
    if(r < 0){
        return r;
    }

    int rc = -ENOENT;
    for(int i = 0; i < mem->pending_requests_table.count; i++){
        if(mem->pending_requests_table.entries[i].id == id){
            mem->pending_requests_table.entries[i].status = st;
            rc = 0;
            break;
        }
    }

    sem_unlock(sem);
    return rc;
}


/*
Marks as FAILED all requests sent by user. 
Used when user's session is terminated, to avoid leaving pending requests in the table.
Returns number of failed requests or -errno.
*/
int shm_requests_set_failed(shared_memory_t *mem, int sem, const char *user){
    int r = sem_lock(sem);
    if(r < 0){
        return r;
    }

    int n= 0;
    for(int i = 0; i < mem->pending_requests_table.count; i++){
        transfer_request_entry_t *entry = &mem->pending_requests_table.entries[i];

        if(entry->status == PENDING && strcmp(entry->source_username, user) == 0){
            entry->status = FAILED;
            n++;
        }
    }

    sem_unlock(sem);
    return n;

}


/*
Copy in out pending requests for dest_user, up to max entries.
Useful during login to collect all pending requests for the user.
Returns number of requests copied, or -errno.
*/
int shm_requests_for_dest(shared_memory_t *mem, int sem, const char *user, transfer_request_entry_t *out, int max){

    int r = sem_lock(sem);
    if(r < 0){
        return r;
    }

    int n = 0;
    for(int i = 0; i < mem->pending_requests_table.count && n < max; i++){
        transfer_request_entry_t *entry = &mem->pending_requests_table.entries[i];

        if(entry->status == PENDING && strcmp(entry->dest_username, user) == 0){
            out[n++] = *entry;
        }
    }

    sem_unlock(sem);
    return n;

}