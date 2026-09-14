// Windows-compatible version of the railway system simulation
// Uses Windows threads and synchronization primitives
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdbool.h>
#include <time.h>

#define NUM_PLATFORMS 5
#define NUM_TRAINS 12
#define NUM_APPROACH_LINES 2
#define NUM_DEPARTURE_LINES 2
#define VIRTUAL_START_HOUR 12

// --- Custom Semaphore Implementation (Windows) ---
typedef struct {
    HANDLE sem;
} Semaphore;
void sem_init(Semaphore* s, int value) { s->sem = CreateSemaphore(NULL, value, 100, NULL); }
void sem_wait(Semaphore* s) { WaitForSingleObject(s->sem, INFINITE); }
void sem_post(Semaphore* s) { ReleaseSemaphore(s->sem, 1, NULL); }
void sem_destroy(Semaphore* s) { CloseHandle(s->sem); }

// --- Global Simulation Clock & State ---
time_t real_start_time;
Semaphore approach_lines[2];
Semaphore departure_lines[2];
bool platform_busy[NUM_PLATFORMS] = {false};
int waiting_for_platform[2][2] = {{0}};
CRITICAL_SECTION station_lock;
HANDLE platform_cond[2][2];

// --- Enums and Structs ---
typedef enum { UP, DOWN } Direction;
const char* DIRECTION_NAMES[] = {"UP", "DOWN"};
typedef enum { EXPRESS_PASS, EXPRESS_HALT, LOCAL_STOP } TrainBehavior;
const char* BEHAVIOR_NAMES[] = {"EXPRESS_PASS", "EXPRESS_HALT", "LOCAL_STOP"};
typedef enum { UP_ONLY, DOWN_ONLY, MIXED } PlatformType;

// --- UPDATED PLATFORM LAYOUT ---
const PlatformType PLATFORM_MAP[NUM_PLATFORMS] = {UP_ONLY, UP_ONLY, MIXED, DOWN_ONLY, DOWN_ONLY};

typedef struct {
    int id; Direction direction; TrainBehavior behavior;
    double scheduled_arrival_min, scheduled_departure_min;
    double time_entered_approach, time_docked, time_entered_departure, actual_departure;
    int platform_used; bool dispatched;
} Train;

// --- Helper Functions ---
void format_time(char* buffer, size_t len, double elapsed_minutes) {
    int total_seconds = (int)(elapsed_minutes * 60);
    int hour = VIRTUAL_START_HOUR + (total_seconds / 3600);
    int minute = (total_seconds % 3600) / 60;
    int second = total_seconds % 60;
    snprintf(buffer, len, "%02d:%02d:%02d", hour, minute, second);
}
double get_elapsed_minutes() { return difftime(time(NULL), real_start_time); }

// --- UPDATED FIND PLATFORM LOGIC ---
int find_platform(Direction dir) {
    for (int i = 0; i < NUM_PLATFORMS; i++) {
        if (!platform_busy[i]) {
            if ((dir == UP && PLATFORM_MAP[i] == UP_ONLY) || (dir == DOWN && PLATFORM_MAP[i] == DOWN_ONLY)) {
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
    Train* train = (Train*)args;
    char time_buf[10];
    char train_desc[100];
    snprintf(train_desc, sizeof(train_desc), "Train %2d (%s %-12s)", train->id, DIRECTION_NAMES[train->direction], BEHAVIOR_NAMES[train->behavior]);

    sem_wait(&approach_lines[train->direction]);
    train->time_entered_approach = get_elapsed_minutes();
    format_time(time_buf, sizeof(time_buf), train->time_entered_approach);
    printf("[%s] %s -> Entered APPROACH zone.\n", time_buf, train_desc);
    Sleep(1000);

    EnterCriticalSection(&station_lock);
    format_time(time_buf, sizeof(time_buf), get_elapsed_minutes());
    printf("[%s] %s -> Arrived at station, needs platform.\n", time_buf, train_desc);

    bool is_express = (train->behavior != LOCAL_STOP);
    waiting_for_platform[train->direction][is_express]++;
    int platform_idx = -1;
    while ((platform_idx = find_platform(train->direction)) == -1 || (!is_express && waiting_for_platform[train->direction][1] > 0)) {
        LeaveCriticalSection(&station_lock);
        WaitForSingleObject(platform_cond[train->direction][is_express], INFINITE);
        EnterCriticalSection(&station_lock);
    }

    sem_post(&approach_lines[train->direction]);
    waiting_for_platform[train->direction][is_express]--;
    platform_busy[platform_idx] = true;
    train->platform_used = platform_idx + 1;
    train->time_docked = get_elapsed_minutes();
    format_time(time_buf, sizeof(time_buf), train->time_docked);
    printf("[%s] ✅ %s -> DOCKED on Platform %d. (Approach line freed)\n", time_buf, train_desc, train->platform_used);
    LeaveCriticalSection(&station_lock);

    double halt_duration_min;
    if (train->behavior == EXPRESS_HALT) halt_duration_min = 10.0;
    else if (train->behavior == LOCAL_STOP) halt_duration_min = 2.0;
    else halt_duration_min = 1.0;
    Sleep((DWORD)(halt_duration_min * 1000));

    double time_after_halt = get_elapsed_minutes();
    if (time_after_halt < train->scheduled_departure_min) {
        double wait_needed = train->scheduled_departure_min - time_after_halt;
        format_time(time_buf, sizeof(time_buf), get_elapsed_minutes());
        printf("[%s] %s -> Finished halt, must wait %.1f min for scheduled departure.\n",
               time_buf, train_desc, wait_needed);
        Sleep((DWORD)(wait_needed * 1000));
    }

    sem_wait(&departure_lines[train->direction]);
    train->time_entered_departure = get_elapsed_minutes();

    EnterCriticalSection(&station_lock);
    platform_busy[platform_idx] = false;
    format_time(time_buf, sizeof(time_buf), train->time_entered_departure);
    printf("[%s] %s -> Left platform, entered DEPARTURE zone.\n", time_buf, train_desc);
    for (int d = 0; d < 2; d++) for (int e = 0; e < 2; e++) if (waiting_for_platform[d][e] > 0) SetEvent(platform_cond[d][e]);
    LeaveCriticalSection(&station_lock);

    Sleep(1000);

    train->actual_departure = get_elapsed_minutes();
    format_time(time_buf, sizeof(time_buf), train->actual_departure);
    printf("[%s] %s -> Exited system.\n", time_buf, train_desc);
    sem_post(&departure_lines[train->direction]);
    return 0;
}

int main() {
    Train schedule[NUM_TRAINS];
    Train base_schedule[NUM_TRAINS] = {
        {1, UP, LOCAL_STOP, 1},
        {2, DOWN, EXPRESS_HALT, 2},
        {3, UP, EXPRESS_PASS, 3},
        {4, DOWN, LOCAL_STOP, 4},
        {5, UP, EXPRESS_HALT, 4},
        {6, DOWN, EXPRESS_HALT, 5},
        {7, UP, LOCAL_STOP, 5},
        {8, DOWN, LOCAL_STOP, 6},
        {9, UP, EXPRESS_HALT, 7},
        {10, UP, LOCAL_STOP, 8},
        {11, DOWN, EXPRESS_PASS, 9},
        {12, DOWN, LOCAL_STOP, 10}
    };

    for (int i = 0; i < NUM_TRAINS; i++) {
        schedule[i] = base_schedule[i];
        double halt = 0;
        if (schedule[i].behavior == EXPRESS_HALT) halt = 10.0;
        else if (schedule[i].behavior == LOCAL_STOP) halt = 2.0;
        else if (schedule[i].behavior == EXPRESS_PASS) halt = 1.0;
        schedule[i].scheduled_departure_min = schedule[i].scheduled_arrival_min + halt;
    }

    HANDLE threads[NUM_TRAINS];
    real_start_time = time(NULL);
    int dispatched_count = 0;
    char time_buf[10];

    sem_init(&approach_lines[UP], NUM_APPROACH_LINES); sem_init(&approach_lines[DOWN], NUM_APPROACH_LINES);
    sem_init(&departure_lines[UP], NUM_DEPARTURE_LINES); sem_init(&departure_lines[DOWN], NUM_DEPARTURE_LINES);
    InitializeCriticalSection(&station_lock);
    for(int i=0; i<2; i++) for(int j=0; j<2; j++) platform_cond[i][j] = CreateEvent(NULL, FALSE, FALSE, NULL);

    format_time(time_buf, sizeof(time_buf), get_elapsed_minutes());
    printf("[%s] Station system is online. Dispatching trains...\n\n", time_buf);

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

    printf("\n--- FINAL PERFORMANCE REPORT ---\n");
    printf("ID | Train Type      | Dir | Sched Arr | Sched Dep | Actual Arr | Actual   | Actual Dep | Actual Dep | Total Delay\n");
    printf("   |                 |     | (Station) | (Station) | (Approach) | Dock     | (Platform) | (System)   | (min)\n");
    printf("---|-----------------|-----|-----------|-----------|------------|----------|------------|------------|------------\n");

    for (int i = 0; i < NUM_TRAINS; i++) {
        Train t = schedule[i];
        char eta_buf[10], edt_buf[10], appr_buf[10], dock_buf[10], dep_buf[10], adt_buf[10];
        format_time(eta_buf, sizeof(eta_buf), t.scheduled_arrival_min);
        format_time(edt_buf, sizeof(edt_buf), t.scheduled_departure_min);
        format_time(appr_buf, sizeof(appr_buf), t.time_entered_approach);
        format_time(dock_buf, sizeof(dock_buf), t.time_docked);
        format_time(dep_buf, sizeof(dep_buf), t.time_entered_departure);
        format_time(adt_buf, sizeof(adt_buf), t.actual_departure);
        double total_delay = t.actual_departure - t.scheduled_departure_min;

        printf("%-2d | %-15s | %-3s | %s   | %s   | %s   | %s | %s   | %s   | %-5.1f %s\n",
                t.id, BEHAVIOR_NAMES[t.behavior], DIRECTION_NAMES[t.direction],
                eta_buf, edt_buf, appr_buf, dock_buf, dep_buf, adt_buf,
                total_delay, total_delay > 0.1 ? "(DELAYED)" : "");
    }

    sem_destroy(&approach_lines[UP]); sem_destroy(&approach_lines[DOWN]);
    sem_destroy(&departure_lines[UP]); sem_destroy(&departure_lines[DOWN]);
    DeleteCriticalSection(&station_lock);
    for(int i=0; i<2; i++) for(int j=0; j<2; j++) CloseHandle(platform_cond[i][j]);

    return 0;
}
