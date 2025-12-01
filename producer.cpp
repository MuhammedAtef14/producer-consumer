#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include <semaphore.h>
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
#define SEM_MUTEX 0
#define SEM_EMPTY 1
#define SEM_FULL  2

const char *SHM_KEY_FILE = "sharedfile"; 
const char SEM_PROJ_ID = 'S';            
const char SHM_PROJ_ID = 'M'; 

using namespace std;

//semaphores 
struct sembuf make_op(short num, short op, short flg) {
    struct sembuf s;
    s.sem_num = num;
    s.sem_op  = op;
    s.sem_flg = flg;
    return s;
}

union semun {
    int val;                
    struct semid_ds *buf;   
    unsigned short *array;
};

//Shared memory and item structs for reading data from shared memory
struct Item {
    char name[11];   
    double price;    
};

struct SharedMemory {
    Item buffer[100];
    size_t bufferSize;
    int in;
    int out;
};

//Formatting tim stamps
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

void produce(SharedMemory* shm, char* commName, double priceMean, double priceSrdDev, int sleepInterval, int bufferSize) {
    std::default_random_engine generator;
    std::normal_distribution<double> distribution(priceMean, priceSrdDev);

    key_t sem_key = ftok(SHM_KEY_FILE, SEM_PROJ_ID);
    if (sem_key == -1) { 
        perror("ftok(sem)"); 
        exit(1); 
    }
    int semid = semget(sem_key, 3, 0666);
    if (semid < 0) { 
        perror("semget (consumer) - semaphores not initialized"); 
        exit(1); 
    }

    if (shm->bufferSize == 0 || shm->bufferSize > 100) {
        perror("Invalid shm->bufferSize\n");
        exit(1);
    }
    

    struct sembuf opP_mutex = make_op(SEM_MUTEX, -1, SEM_UNDO);
    struct sembuf opV_mutex = make_op(SEM_MUTEX, +1, SEM_UNDO);
    struct sembuf opP_empty = make_op(SEM_EMPTY, -1, SEM_UNDO);
    struct sembuf opV_full  = make_op(SEM_FULL, +1, SEM_UNDO);

    //Producer logic
    while (1) {
        double price = distribution(generator);
        price = std::round(price * 100.0) / 100.0;
        if (price < 0) price = 0;
        std::cerr << "\n" << getTime() << " " << commName 
                  << ": generating a new value " << price << "\n";
        
        // Wait for empty slot
        semop(semid, &opP_empty, 1);        
        std::cerr << getTime() << " " << commName 
                  << ": trying to get mutex on shared buffer\n";
        //Wait for MUTEX
        semop(semid, &opP_mutex, 1);  

        //INserting a item
        int index = shm->in % shm->bufferSize;
        strncpy(shm->buffer[index].name, commName, MAX_NAME - 1);
        shm->buffer[index].name[MAX_NAME- 1] = '\0';
        shm->buffer[index].price = price;
        shm->in++;
        std::cerr << getTime() << " " << commName 
                  << ": placing " << price << " on shared buffer\n";
        
        //mutex signal
        semop(semid, &opV_mutex, 1);  
        //full signal
        semop(semid, &opV_full, 1);  
        std::cerr << getTime() << " " << commName 
                  << ": sleeping for " << sleepInterval << " ms\n\n";
        usleep(sleepInterval * 1000);
    }
}

int main(int argc, char* argv[]) {
    
    if (argc != 6) {
        printf("Error in input Arguments!\n");

    }
    int N = atoi(argv[5]); // buffer size requested by user

    //shared memory
    key_t shm_key = ftok(SHM_KEY_FILE, SHM_PROJ_ID);
    if (shm_key == (key_t)-1) { perror("ftok(shm)"); exit(1); }

    int shmid = shmget(shm_key, sizeof(SharedMemory), 0666);
    bool weCreatedShm = false;
    if (shmid == -1) {
        shmid = shmget(shm_key, sizeof(SharedMemory), IPC_CREAT | IPC_EXCL | 0666);
        if (shmid == -1) {
            if (errno == EEXIST) {
                shmid = shmget(shm_key, sizeof(SharedMemory), 0666);
                if (shmid == -1) {
                    perror("shmget reopen after EEXIST");
                    exit(1);
                }
            } else {
                perror("shmget create");
                exit(1);
            }
        } else {
            weCreatedShm = true;
        }
    }

    // Attach
    SharedMemory* shm = (SharedMemory*) shmat(shmid, NULL, 0);
    if (shm == (void*)-1) {
        perror("shmat");
        exit(1);
    }


    bool need_init = false;
    if ( weCreatedShm || shm->bufferSize == 0) need_init = true;

    if (need_init) {
        if (N <= 0 || N > 100) {
            fprintf(stderr, "Invalid requested buffer size N=%d (must be 1..100)\n", N);
            shmdt(shm);
            exit(1);
        }
        shm->in = 0;
        shm->out = 0;
        shm->bufferSize = (size_t) N;
        for (size_t i=0;i<shm->bufferSize;i++) {
            shm->buffer[i].name[0] = '\0';
            shm->buffer[i].price = 0.0;
        }
    }

    int semid = -1;
    key_t sem_key = ftok(SHM_KEY_FILE, SEM_PROJ_ID);
    if (sem_key == (key_t)-1) { perror("ftok(sem)"); shmdt(shm); exit(1); }

    semid = semget(sem_key, 3, IPC_CREAT | IPC_EXCL | 0666);
    bool weCreatedSem = false;
    if (semid == -1) {
        if (errno == EEXIST) {
            
            semid = semget(sem_key, 3, 0666);
            if (semid == -1) { perror("semget reopen"); shmdt(shm); exit(1); }
        } else {
            semid = semget(sem_key, 3, 0666);
            if (semid == -1) { perror("semget"); shmdt(shm); exit(1); }
        }
    } else {
        // we created the semaphore set 
        weCreatedSem = true;
    }

    if (weCreatedShm && weCreatedSem) {
        unsigned short vals[3];
        vals[0] = 1;                         // MUTEX
        vals[1] = (unsigned short) shm->bufferSize; // EMPTY = N
        vals[2] = 0;                         // FULL

        union semun arg;
        arg.array = vals;
        if (semctl(semid, 0, SETALL, arg) == -1) {
            perror("semctl SETALL");
            semctl(semid, 0, IPC_RMID);
            shmdt(shm);
            exit(1);
        }
    }
    if ((int)shm->bufferSize != N) {
        fprintf(stderr, "Warning: requested N=%d differs from shm->bufferSize=%zu. Using shm value.\n", N, shm->bufferSize);
    }

    produce(shm, strdup(argv[1]), std::atof(argv[2]), std::stod(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]));
    return 0;
};
