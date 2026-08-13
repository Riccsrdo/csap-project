#include"sh_mem.h"

int sem_lock(int sem_id){
    struct sembuf sb;
    sb.sem_num = 0; // semaphore index
    sb.sem_op = -1; // decrement operation (lock)
    sb.sem_flg = SEM_UNDO; // automatically undo the operation if the process exits

    while(semop(sem_id, &sb, 1) < 0){
        if(errno == EINTR) continue; // interrupted by signal, retry
        return -errno; // error
    }
    return 0; // success
}

int sem_unlock(int sem_id){
    struct sembuf sb;
    sb.sem_num = 0; // semaphore index
    sb.sem_op = 1; // increment operation (unlock)
    sb.sem_flg = SEM_UNDO; // automatically undo the operation if the process exits

    while(semop(sem_id, &sb, 1) < 0){
        if(errno == EINTR) continue; // interrupted by signal, retry
        return -errno; // error
    }
    return 0; // success
}

int setup_shmem(int *smh_id, int *sem_id, shared_memory_t **shm_ptr_out, gid_t owner_gid){
    // I used 0600 permissions to ensure only the owner can read/write (the father)
    int shm_id_temp = shmget(IPC_PRIVATE, sizeof(shared_memory_t), IPC_CREAT  | 0600);
    if(shm_id_temp < 0){
        return -errno;
    }

    // attach to the shared memory segment
    // NULL means the system chooses the address at which to attach the segment
    shared_memory_t *shm_ptr = (shared_memory_t *)shmat(shm_id_temp, NULL, 0);
    if(shm_ptr == (void *)-1){
        int saved = errno;
        shmctl(shm_id_temp, IPC_RMID, NULL); // remove the shared memory
        return -saved;
    }

    memset(shm_ptr, 0, sizeof(shared_memory_t)); // initialize the shared memory to zero

    // I use 0660 to ensure also children belonging in the "csap_group" can modify the semaphore
    int sem_id_temp = semget(IPC_PRIVATE, 1, IPC_CREAT | 0660);
    if(sem_id_temp < 0){
        int saved = errno;
        shmdt(shm_ptr); // detach from the shared memory
        shmctl(shm_id_temp, IPC_RMID, NULL); // remove the shared memory
        return -saved;
    }

    // initialize the union for semctl
    union semun arg;

    // initialize the semaphore to 1 (unlocked)
    arg.val = 1;

    struct semid_ds sem_info;
    arg.buf = &sem_info;


    if(semctl(sem_id_temp, 0, IPC_STAT, arg) == 0){
        sem_info.sem_perm.gid = owner_gid; // set the group ID of the semaphore to the server's group ID, otherwise it's created with root group
        sem_info.sem_perm.mode = 0660; // set the permissions of the semaphore to 0660
        if(semctl(sem_id_temp, 0, IPC_SET, arg) < 0){
            int saved = errno;
            shmdt(shm_ptr); // detach from the shared memory
            shmctl(shm_id_temp, IPC_RMID, NULL); // remove the shared memory
            semctl(sem_id_temp, 0, IPC_RMID); // remove the semaphore
            return -saved;
        }
    }

    if(semctl(sem_id_temp, 0, SETVAL, arg) < 0){
        int saved = errno;
        shmdt(shm_ptr); // detach from the shared memory
        shmctl(shm_id_temp, IPC_RMID, NULL); // remove the shared memory
        semctl(sem_id_temp, 0, IPC_RMID); // remove the semaphore
        return -saved;
    }

    *shm_ptr_out = shm_ptr;
    *smh_id = shm_id_temp;
    *sem_id = sem_id_temp;

    // mark the shared memory for deletion, it will be deleted when all processes detach
    shmctl(shm_id_temp, 0, IPC_RMID); 
    
    return 0; // success
}

void cleanup_shmem(int shm_id, int sem_id, shared_memory_t *shm_ptr){
    if(shm_ptr != NULL){
        shmdt(shm_ptr); // detach from the shared memory
    }
    if(shm_id >= 0){
        shmctl(shm_id, IPC_RMID, NULL); // remove the shared memory
    }
    if(sem_id >= 0){
        semctl(sem_id, 0, IPC_RMID); // remove the semaphore
    }
}