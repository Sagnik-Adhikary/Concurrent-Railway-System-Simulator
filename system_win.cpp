// Windows-compatible C++ version of the railway system simulation
// Uses Windows threads and synchronization primitives
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <windows.h>
#include <ctime>

#define NUM_PLATFORMS 5
#define NUM_TRAINS 12
#define NUM_APPROACH_LINES 2
#define NUM_DEPARTURE_LINES 2
#define VIRTUAL_START_HOUR 12

// --- Custom Semaphore Class (Windows) ---
class Semaphore {
private:
    HANDLE sem;
public:
    Semaphore(int value) { sem = CreateSemaphore(NULL, value, 100, NULL); }
    ~Semaphore() { CloseHandle(sem); }
    void wait() { WaitForSingleObject(sem, INFINITE); }
    void post() { ReleaseSemaphore(sem, 1, NULL); }
};

// --- Enums ---
enum Direction { UP, DOWN };
const std::string DIRECTION_NAMES[] = {"UP", "DOWN"};

enum TrainBehavior { EXPRESS_PASS, EXPRESS_HALT, LOCAL_STOP };
const std::string BEHAVIOR_NAMES[] = {"EXPRESS_PASS", "EXPRESS_HALT", "LOCAL_STOP"};

enum PlatformType { UP_ONLY, DOWN_ONLY, MIXED };

// --- Global Simulation State ---
time_t real_start_time;
Semaphore approach_lines[2] = {Semaphore(NUM_APPROACH_LINES), Semaphore(NUM_APPROACH_LINES)};
Semaphore departure_lines[2] = {Semaphore(NUM_DEPARTURE_LINES), Semaphore(NUM_DEPARTURE_LINES)};
bool platform_busy[NUM_PLATFORMS] = {false};
int waiting_for_platform[2][2] = {{0}};
CRITICAL_SECTION station_lock;
HANDLE platform_cond[2][2];

// --- Platform Layout ---
const PlatformType PLATFORM_MAP[NUM_PLATFORMS] = {UP_ONLY, UP_ONLY, MIXED, DOWN_ONLY, DOWN_ONLY};

// --- Train Class ---
class Train {
public:
    int id;
    Direction direction;
    TrainBehavior behavior;
    double scheduled_arrival_min, scheduled_departure_min;
    double time_entered_approach, time_docked, time_entered_departure, actual_departure;
    int platform_used;
    bool dispatched;

    Train() : id(0), direction(UP), behavior(LOCAL_STOP), scheduled_arrival_min(0), 
              scheduled_departure_min(0), time_entered_approach(0), time_docked(0),
              time_entered_departure(0), actual_departure(0), platform_used(0), dispatched(false) {}

    Train(int id, Direction dir, TrainBehavior beh, double arr) 
        : id(id), direction(dir), behavior(beh), scheduled_arrival_min(arr),
          time_entered_approach(0), time_docked(0), time_entered_departure(0),
          actual_departure(0), platform_used(0), dispatched(false) {
        double halt = 0;
        if (behavior == EXPRESS_HALT) halt = 10.0;
        else if (behavior == LOCAL_STOP) halt = 2.0;
        else if (behavior == EXPRESS_PASS) halt = 1.0;
        scheduled_departure_min = scheduled_arrival_min + halt;
    }
};

// --- Helper Functions ---
std::string format_time(double elapsed_minutes) {
    int total_seconds = static_cast<int>(elapsed_minutes * 60);
    int hour = VIRTUAL_START_HOUR + (total_seconds / 3600);
    int minute = (total_seconds % 3600) / 60;
    int second = total_seconds % 60;
    
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hour << ":"
        << std::setfill('0') << std::setw(2) << minute << ":"
        << std::setfill('0') << std::setw(2) << second;
    return oss.str();
}

double get_elapsed_minutes() { 
    return difftime(time(NULL), real_start_time); 
}

// --- Find Platform Logic ---
int find_platform(Direction dir) {
    for (int i = 0; i < NUM_PLATFORMS; i++) {
        if (!platform_busy[i]) {
            if ((dir == UP && PLATFORM_MAP[i] == UP_ONLY) || 
                (dir == DOWN && PLATFORM_MAP[i] == DOWN_ONLY)) {
                return i;
            }
        }
    }
    for (int i = 0; i < NUM_PLATFORMS; i++) {
        if (!platform_busy[i] && PLATFORM_MAP[i] == MIXED) {
            return i;
        }
    }
    return -1;
}

// --- Main Thread Logic ---
DWORD WINAPI train_thread(LPVOID args) {
    Train* train = static_cast<Train*>(args);
    
    std::string train_desc = "Train " + std::to_string(train->id) + " (" + 
                             DIRECTION_NAMES[train->direction] + " " + 
                             BEHAVIOR_NAMES[train->behavior] + ")";

    approach_lines[train->direction].wait();
    train->time_entered_approach = get_elapsed_minutes();
    std::cout << "[" << format_time(train->time_entered_approach) << "] " 
              << train_desc << " -> Entered APPROACH zone.\n";
    Sleep(1000);

    EnterCriticalSection(&station_lock);
    std::cout << "[" << format_time(get_elapsed_minutes()) << "] " 
              << train_desc << " -> Arrived at station, needs platform.\n";

    bool is_express = (train->behavior != LOCAL_STOP);
    waiting_for_platform[train->direction][is_express]++;
    int platform_idx = -1;
    
    while ((platform_idx = find_platform(train->direction)) == -1 || 
           (!is_express && waiting_for_platform[train->direction][1] > 0)) {
        LeaveCriticalSection(&station_lock);
        WaitForSingleObject(platform_cond[train->direction][is_express], INFINITE);
        EnterCriticalSection(&station_lock);
    }

    approach_lines[train->direction].post();
    waiting_for_platform[train->direction][is_express]--;
    platform_busy[platform_idx] = true;
    train->platform_used = platform_idx + 1;
    train->time_docked = get_elapsed_minutes();
    std::cout << "[" << format_time(train->time_docked) << "] ✅ " 
              << train_desc << " -> DOCKED on Platform " << train->platform_used 
              << ". (Approach line freed)\n";
    LeaveCriticalSection(&station_lock);

    double halt_duration_min;
    if (train->behavior == EXPRESS_HALT) halt_duration_min = 10.0;
    else if (train->behavior == LOCAL_STOP) halt_duration_min = 2.0;
    else halt_duration_min = 1.0;
    Sleep(static_cast<DWORD>(halt_duration_min * 1000));

    double time_after_halt = get_elapsed_minutes();
    if (time_after_halt < train->scheduled_departure_min) {
        double wait_needed = train->scheduled_departure_min - time_after_halt;
        std::cout << "[" << format_time(get_elapsed_minutes()) << "] " 
                  << train_desc << " -> Finished halt, must wait " 
                  << std::fixed << std::setprecision(1) << wait_needed 
                  << " min for scheduled departure.\n";
        Sleep(static_cast<DWORD>(wait_needed * 1000));
    }

    departure_lines[train->direction].wait();
    train->time_entered_departure = get_elapsed_minutes();

    EnterCriticalSection(&station_lock);
    platform_busy[platform_idx] = false;
    std::cout << "[" << format_time(train->time_entered_departure) << "] " 
              << train_desc << " -> Left platform, entered DEPARTURE zone.\n";
    for (int d = 0; d < 2; d++) 
        for (int e = 0; e < 2; e++) 
            if (waiting_for_platform[d][e] > 0) 
                SetEvent(platform_cond[d][e]);
    LeaveCriticalSection(&station_lock);

    Sleep(1000);

    train->actual_departure = get_elapsed_minutes();
    std::cout << "[" << format_time(train->actual_departure) << "] " 
              << train_desc << " -> Exited system.\n";
    departure_lines[train->direction].post();
    return 0;
}

int main() {
    std::vector<Train> schedule = {
        Train(1, UP, LOCAL_STOP, 1),
        Train(2, DOWN, EXPRESS_HALT, 2),
        Train(3, UP, EXPRESS_PASS, 3),
        Train(4, DOWN, LOCAL_STOP, 4),
        Train(5, UP, EXPRESS_HALT, 4),
        Train(6, DOWN, EXPRESS_HALT, 5),
        Train(7, UP, LOCAL_STOP, 5),
        Train(8, DOWN, LOCAL_STOP, 6),
        Train(9, UP, EXPRESS_HALT, 7),
        Train(10, UP, LOCAL_STOP, 8),
        Train(11, DOWN, EXPRESS_PASS, 9),
        Train(12, DOWN, LOCAL_STOP, 10)
    };

    HANDLE threads[NUM_TRAINS];
    real_start_time = time(NULL);
    int dispatched_count = 0;

    InitializeCriticalSection(&station_lock);
    for(int i = 0; i < 2; i++) 
        for(int j = 0; j < 2; j++) 
            platform_cond[i][j] = CreateEvent(NULL, FALSE, FALSE, NULL);

    std::cout << "[" << format_time(get_elapsed_minutes()) 
              << "] Station system is online. Dispatching trains...\n\n";

    while(dispatched_count < NUM_TRAINS) {
        for (int i = 0; i < NUM_TRAINS; i++) {
            if (!schedule[i].dispatched && get_elapsed_minutes() >= schedule[i].scheduled_arrival_min) {
                schedule[i].dispatched = true;
                threads[i] = CreateThread(NULL, 0, train_thread, &schedule[i], 0, NULL);
                dispatched_count++;
            }
        }
        Sleep(100);
    }

    WaitForMultipleObjects(NUM_TRAINS, threads, TRUE, INFINITE);
    for (int i = 0; i < NUM_TRAINS; i++) CloseHandle(threads[i]);

    std::cout << "\n--- FINAL PERFORMANCE REPORT ---\n";
    std::cout << "ID | Train Type      | Dir | Sched Arr | Sched Dep | Actual Arr | Actual   | Actual Dep | Actual Dep | Total Delay\n";
    std::cout << "   |                 |     | (Station) | (Station) | (Approach) | Dock     | (Platform) | (System)   | (min)\n";
    std::cout << "---|-----------------|-----|-----------|-----------|------------|----------|------------|------------|------------\n";

    for (const auto& t : schedule) {
        double total_delay = t.actual_departure - t.scheduled_departure_min;
        
        std::cout << std::setw(2) << t.id << " | "
                  << std::setw(15) << std::left << BEHAVIOR_NAMES[t.behavior] << " | "
                  << std::setw(3) << DIRECTION_NAMES[t.direction] << " | "
                  << format_time(t.scheduled_arrival_min) << "   | "
                  << format_time(t.scheduled_departure_min) << "   | "
                  << format_time(t.time_entered_approach) << "   | "
                  << format_time(t.time_docked) << " | "
                  << format_time(t.time_entered_departure) << "   | "
                  << format_time(t.actual_departure) << "   | "
                  << std::fixed << std::setprecision(1) << std::setw(5) << total_delay 
                  << (total_delay > 0.1 ? " (DELAYED)" : "") << "\n";
    }

    DeleteCriticalSection(&station_lock);
    for(int i = 0; i < 2; i++) 
        for(int j = 0; j < 2; j++) 
            CloseHandle(platform_cond[i][j]);

    return 0;
}