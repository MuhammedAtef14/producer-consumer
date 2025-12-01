#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <map>
#include <vector>
#include <deque>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#define SEM_MUTEX 0
#define SEM_EMPTY 1
#define SEM_FULL  2
#define MAX_NAME 11
using namespace std;
const char *SHM_KEY_FILE = "sharedfile"; 
const char SEM_PROJ_ID = 'S';            
const char SHM_PROJ_ID = 'M';             


union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

struct sembuf make_op(short num, short op, short flg) {
    struct sembuf s;
    s.sem_num = num;
    s.sem_op  = op;
    s.sem_flg = flg;
    return s;
}



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

// Shared memory and item structs for reading data from shared memory




struct PricesHistory {
    deque<double> prices; 
};

struct CommodityStats {
    PricesHistory history;
    double latest_price = 0.0;
    double current_average = 0.0;
    string commodity_state = "IDLE";    // "IDLE", "UP", "DOWN"
};

//map to get commodity name from id
map<string,int> mp={
    {"ALUMINIUM",0},
    {"COPPER",1},{"COTTON",2},{"CRUDEOIL",3},{"GOLD",4},{"LEAD",5},{"MENTHAOIL",6},{"NATURALGAS",7},{"NICKEL",8},{"SILVER",9},{"ZINC",10}
};

struct CommodityStats * arr[11];

void initializeCommodityStatsArray() {
    for (int i = 0; i < 11; ++i) {
        arr[i] = new CommodityStats();
    }
}

void AddPrice(CommodityStats &stats, double price, int lookback = 4) {
    
    if(stats.history.prices.size()==lookback+1)
    {
        stats.history.prices.pop_front();//release the oldest price
    }
    stats.history.prices.push_back(price); //add the new price
    double sum=0.0;
    for(auto &p:stats.history.prices)
    {
        sum+=p;
    }
    double avrg=sum/ double(stats.history.prices.size());
    //do checks for state 
    if(avrg>stats.current_average)
    {
        stats.commodity_state="UP";
    }
    else if(avrg<stats.current_average)
    {
        stats.commodity_state="DOWN";
    }
    else {
        stats.commodity_state="IDLE";
    }
    stats.latest_price=price;
    stats.current_average=avrg;     
}



void printDashboard() {
    printf("\033[2J\033[1;1H"); 

    const vector<string> commodity_names = {
        "ALUMINIUM", "COPPER", "COTTON", "CRUDEOIL", 
        "GOLD", "LEAD", "MENTHAOIL", "NATURALGAS", 
        "NICKEL", "SILVER", "ZINC"
    };


    std::cout << "========================================================\n";
    std::cout << "| Currency    |  Price  | AvgPrice | Trend\n";
    std::cout << "========================================================\n";
    
    for (const std::string& name : commodity_names) {
        
        // Use the map to get the correct index (0-10) for the fixed array 'arr'
        int index = mp[name];
        
        // Get the statistics pointer for the current commodity
        CommodityStats* stats_ptr = arr[index];

        if (stats_ptr == nullptr) {
            printf("| %-11s |  %7.2lf |  %7.2lf | \n", 
                   name.c_str(), 
                   0.00, 
                   0.00);
            continue;
        }

        // Get the current commodity statistics
        CommodityStats& stats = *stats_ptr;
        
        const char* arrow_symbol = "";
        const char* color_code = ""; 
        
        if (stats.commodity_state == "UP") {
            // Green Up Arrow (For UP trend)
            arrow_symbol = " \033[32m\u2191\033[0m"; // \u2191 is an up arrow
        } else if (stats.commodity_state == "DOWN") {
            // Red Down Arrow (For DOWN trend)
            arrow_symbol = " \033[31m\u2193\033[0m"; // \u2193 is a down arrow
        } else {
            // Neutral symbol or blank for IDLE/EQUAL
            arrow_symbol = ""; 
        }

        // --- Print the Table Row ---
        printf("| %-11s |  %7.2lf |  %7.2lf | %-5s\n", 
               name.c_str(), 
               stats.latest_price, // Current Price
               stats.current_average, // Average Price (current + past 4)
               arrow_symbol);
    }
    
     cout << "========================================================\n";
    //std::cout << "Last updated: " << std::time(nullptr) << "\n"; // Optional timestamp
    cout.flush(); // Ensure output is immediately displayed
}



//i will create array of commodity stats in local memory after getting the shared memory pointer
//then i will update each commodity stats from the shared memory 


void Consumer_logic(SharedMemory* shm,int BUFFER_SIZE)
{
          
           key_t sem_key = ftok(SHM_KEY_FILE, SEM_PROJ_ID);
           if (sem_key == -1) { perror("ftok(sem)"); exit(1); }
           int semid = semget(sem_key, 3, 0666);
           if (semid < 0) { perror("semget (consumer) - semaphores not initialized"); exit(1); }

           if (shm->bufferSize == 0 || shm->bufferSize > 100) {
           fprintf(stderr, "Invalid shm->bufferSize=%zu\n", shm->bufferSize);
           exit(1);
            }
    

         struct sembuf opP_full  = make_op(SEM_FULL, -1, SEM_UNDO);
         struct sembuf opP_mutex = make_op(SEM_MUTEX, -1, SEM_UNDO);
         struct sembuf opV_mutex = make_op(SEM_MUTEX, +1, SEM_UNDO);
         struct sembuf opV_empty = make_op(SEM_EMPTY, +1, SEM_UNDO);

        while(true)
        {
             
             semop(semid, &opP_full, 1);   //wait Full
             semop(semid, &opP_mutex, 1);
             

             //crtical section  
             Item item = shm->buffer[shm->out];
             shm->out = (shm->out + 1) % shm->bufferSize;
              //end of critical section   
    
             semop(semid, &opV_mutex, 1);
             semop(semid, &opV_empty, 1);


            string commodity_name(item.name);
            for (auto & c: commodity_name) c = toupper(c);

            double price = item.price;

            //validate lw el commodname wrong 

            if(mp.find(commodity_name)==mp.end())  //m4 mawgood felmap el upper case bta3oh
            {
                printf("Invalid commodity name: %s\n", item.name);
                continue; 
            }

            int index = mp[commodity_name];
            CommodityStats* stats_ptr = arr[index];
            if (stats_ptr != nullptr) {
                AddPrice(*stats_ptr, price);
            }
            printDashboard();
        }
    
}

int main(int argc,char * argv[])
{
    if(argc<2) {
        printf("Usage: %s <BUFFER_SIZE>\n", argv[0]);
        exit(1);
    }

    int N = atoi(argv[1]); // buffer size requested by user
    initializeCommodityStatsArray();

    
    FILE *f = fopen(SHM_KEY_FILE, "a");
    if (f) fclose(f);

    
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

    
    Consumer_logic(shm,shm->bufferSize);

    shmdt(shm);
    return 0;
}
