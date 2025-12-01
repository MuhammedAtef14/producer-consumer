// producer.cpp
// Modified to use the same System V IPC scheme as the consumer:
//  - Shared memory key: ftok("sharedfile", 'M') for SHM (consumer uses 'M')
//  - Semaphore key:   ftok("sharedfile", 'S') for semaphore set (consumer uses 'S')
// Semaphores: single set of 3 sems: 0=MUTEX, 1=EMPTY, 2=FULL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include <random>
#include <thread>
#include <chrono>
#include <ctime>
#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <errno.h>

#define MAX_NAME 11

using namespace std;

#define SEM_MUTEX 0
#define SEM_EMPTY 1
#define SEM_FULL  2

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

struct Item {
    char name[MAX_NAME];
    double price;
};

struct SharedMemory {
    Item buffer[100];
    size_t bufferSize;
    int in;
    int out;
};

// semop helper for interrupted syscalls
static void do_semop(int semid, struct sembuf *op, size_t n) {
    while (true) {
        if (semop(semid, op, n) == 0) return;
        if (errno == EINTR) continue;
        perror("semop");
        exit(1);
    }
}

std::string getTime() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    time_t now = ts.tv_sec;
    struct tm *t = localtime(&now);

    char buffer[100];
    snprintf(buffer, sizeof(buffer), "[%02d/%02d/%04d %02d:%02d:%02d]",
             t->tm_mon + 1, t->tm_mday, t->tm_year + 1900,
             t->tm_hour, t->tm_min, t->tm_sec);

    return std::string(buffer);
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <COMMODITY_NAME> <MEAN> <STDDEV> <SLEEP_MS> <BUFFER_SIZE>\n", argv[0]);
        return 1;
    }

    char* commName = argv[1];
    double priceMean = atof(argv[2]);
    double priceSrdDev = atof(argv[3]);
    int sleepInterval = atoi(argv[4]);
    int bufferSizeArg = atoi(argv[5]);

    if (bufferSizeArg <= 0 || bufferSizeArg > 100) {
        fprintf(stderr, "Invalid buffer size (must be 1..100)\n");
        return 1;
    }
    FILE *f = fopen("sharedfile", "a");
if (!f) { perror("fopen sharedfile"); exit(1); }
fclose(f);

    // --- Shared memory (use same ftok file & proj id as consumer)
    key_t shm_key = ftok("sharedfile", 'M'); // consumer uses SHM_PROJ_ID='M'
    if (shm_key == (key_t)-1) { perror("ftok(shm)"); return 1; }

    // create or open SHM (producer can create)
    int shmid = shmget(shm_key, sizeof(SharedMemory), IPC_CREAT | 0666);
    if (shmid < 0) { perror("shmget"); return 1; }

    SharedMemory* shm = (SharedMemory*) shmat(shmid, NULL, 0);
    if (shm == (void*) -1) { perror("shmat"); return 1; }

    // initialize SHM fields if uninitialized (bufferSize==0)
    if (shm->bufferSize == 0) {
        shm->in = 0;
        shm->out = 0;
        shm->bufferSize = (size_t) bufferSizeArg;
        for (size_t i = 0; i < shm->bufferSize; ++i) {
            shm->buffer[i].name[0] = '\0';
            shm->buffer[i].price = 0.0;
        }
        // note: if consumer also tries to create at same time either one will win
    } else {
        // If existent and different, just print a warning and continue using SHM's bufferSize
        if ((int)shm->bufferSize != bufferSizeArg) {
            fprintf(stderr, "Warning: requested N=%d differs from existing shm->bufferSize=%zu. Using shm value.\n",
                    bufferSizeArg, shm->bufferSize);
        }
    }

    // --- Semaphores: use single set with 3 semaphores (same key as consumer's SEM_PROJ_ID 'S')
    key_t sem_key = ftok("sharedfile", 'S'); // consumer uses SEM_PROJ_ID='S'
    if (sem_key == (key_t)-1) { perror("ftok(sem)"); shmdt(shm); return 1; }

    int semid = semget(sem_key, 3, IPC_CREAT | IPC_EXCL | 0666);
    bool we_created_sem = false;
    if (semid == -1) {
        if (errno == EEXIST) {
            semid = semget(sem_key, 3, 0666);
            if (semid == -1) { perror("semget reopen"); shmdt(shm); return 1; }
        } else {
            // try open without EXCL as fallback
            semid = semget(sem_key, 3, 0666);
            if (semid == -1) { perror("semget"); shmdt(shm); return 1; }
        }
    } else {
        we_created_sem = true;
    }

    // initialize semaphores only if we created the set (avoid stomping)
    if (we_created_sem) {
        unsigned short vals[3];
        vals[SEM_MUTEX] = 1;
        vals[SEM_EMPTY] = (unsigned short) shm->bufferSize;
        vals[SEM_FULL] = 0;
        union semun arg;
        arg.array = vals;
        if (semctl(semid, 0, SETALL, arg) == -1) {
            perror("semctl SETALL");
            semctl(semid, 0, IPC_RMID);
            shmdt(shm);
            return 1;
        }
        fprintf(stderr, "Producer: created and initialized semaphore set (semid=%d)\n", semid);
    }

    // Prepare sembuf operations (use correct sem numbers)
    struct sembuf P_mutex = {SEM_MUTEX, -1, 0};
    struct sembuf V_mutex = {SEM_MUTEX, +1, 0};
    struct sembuf P_empty = {SEM_EMPTY, -1, 0};
    struct sembuf V_full  = {SEM_FULL,  +1, 0};

    // Random generator
    std::default_random_engine generator((unsigned)time(NULL) ^ getpid());
    std::normal_distribution<double> distribution(priceMean, priceSrdDev);

    // Producer loop
    while (1) {
        double price = distribution(generator);
        price = std::round(price * 100.0) / 100.0;
        if (price < 0) price = 0.0;

        std::cerr << getTime() << " " << commName
                  << ": generating a new value " << price << "\n";

        // Wait for empty slot (P EMPTY)
        do_semop(semid, &P_empty, 1);

        // Wait for MUTEX
        do_semop(semid, &P_mutex, 1);

        // Critical section: place item
        int index = shm->in % shm->bufferSize;
        // safe copy name and ensure null-termination
        strncpy(shm->buffer[index].name, commName, MAX_NAME - 1);
        shm->buffer[index].name[MAX_NAME - 1] = '\0';
        shm->buffer[index].price = price;
        shm->in++;
        std::cerr << getTime() << " " << commName << ": placed " << price << " at index " << index << "\n";

        // release mutex
        do_semop(semid, &V_mutex, 1);
        // signal full
        do_semop(semid, &V_full, 1);

        // sleep
        usleep((useconds_t)sleepInterval * 1000);
    }

    // detach (unreachable)
    shmdt(shm);
    return 0;
}
