/*
 * task.c - Cooperative Task Scheduler
 *
 * ============================================================================
 * THE TASK SYSTEM: GBA's Answer to Multitasking
 * ============================================================================
 *
 * The GBA has NO operating system, NO threads, and NO multitasking support.
 * But games need to do many things simultaneously: animate sprites, scroll
 * text, process menus, run particle effects, etc.
 *
 * The TASK SYSTEM solves this. A "task" is a function that gets called
 * once per frame. Each task does a SMALL amount of work each frame
 * (like advancing an animation by one step), then returns. This creates
 * the ILLUSION of parallel execution.
 *
 * HOW IT WORKS:
 *
 * 1. Game code calls CreateTask(MyFunc, priority) to register a function.
 * 2. The task system assigns an ID (0-15) and inserts it into a
 *    priority-sorted linked list.
 * 3. Each frame, RunTasks() walks the linked list from head to tail,
 *    calling each active task's function once.
 * 4. When a task is done, it calls DestroyTask(taskId) to remove itself.
 *
 * PRIORITY:
 *   Lower priority NUMBER = runs FIRST (confusingly, "priority 0" is highest).
 *   Tasks with the same priority run in creation order.
 *   Priority determines execution order, NOT importance. A priority-0 task
 *   runs before a priority-10 task each frame.
 *
 * TASK DATA:
 *   Each task has 16 s16 "data" fields (gTasks[id].data[0..15]).
 *   These are general-purpose storage for the task's local state.
 *   For example, an animation task might use:
 *     data[0] = current frame
 *     data[1] = X position
 *     data[2] = Y position
 *     data[3] = timer countdown
 *
 * STATE MACHINE PATTERN:
 *   Most tasks implement multi-frame operations as state machines:
 *     void MyTask(u8 taskId) {
 *         switch (gTasks[taskId].data[0]) {  // data[0] = state
 *         case 0:  // Init
 *             ... setup ...
 *             gTasks[taskId].data[0]++;  // Go to next state
 *             break;
 *         case 1:  // Animate
 *             ... do one frame of animation ...
 *             if (done) gTasks[taskId].data[0]++;
 *             break;
 *         case 2:  // Cleanup
 *             DestroyTask(taskId);
 *             break;
 *         }
 *     }
 *
 * LINKED LIST:
 *   Tasks are stored in an array (gTasks[16]) but linked via prev/next
 *   pointers. This allows O(1) insertion/removal and priority-ordered
 *   iteration without sorting the array.
 *   HEAD_SENTINEL (0xFE) marks the start, TAIL_SENTINEL (0xFF) marks the end.
 *
 * ============================================================================
 */

#include "global.h"
#include "task.h"

/*
 * Sentinel values for the linked list endpoints.
 * HEAD_SENTINEL in a task's 'prev' field means it's the first task.
 * TAIL_SENTINEL in a task's 'next' field means it's the last task.
 * These are NOT valid task IDs - they're special markers.
 */
#define HEAD_SENTINEL 0xFE
#define TAIL_SENTINEL 0xFF

/*
 * The task array. NUM_TASKS = 16 tasks maximum.
 * Stored in IWRAM (COMMON_DATA) for fast access during RunTasks().
 */
COMMON_DATA struct Task gTasks[NUM_TASKS] = {0};

static void InsertTask(u8 newTaskId);
static u8 FindFirstActiveTask();

/**
 * FUNCTION: ResetTasks
 *
 * PURPOSE: Initialize all task slots to inactive/empty state.
 *
 * HOW IT WORKS:
 * Sets every task to inactive with a dummy function, forms a simple
 * linked list where each task points to the next array element.
 * data arrays are zeroed. Called during major state transitions
 * (entering overworld, starting battle, etc.) to clean up all tasks.
 */
void ResetTasks(void)
{
    u8 i;

    for (i = 0; i < NUM_TASKS; i++)
    {
        gTasks[i].isActive = FALSE;
        gTasks[i].func = TaskDummy;       /* Placeholder function (does nothing) */
        gTasks[i].prev = i;               /* Point to self (will be overwritten on insert) */
        gTasks[i].next = i + 1;           /* Point to next slot in array */
        gTasks[i].priority = -1;          /* 0xFF = lowest possible priority */
        memset(gTasks[i].data, 0, sizeof(gTasks[i].data));
    }

    /* Mark the first task as list head and the last as list tail */
    gTasks[0].prev = HEAD_SENTINEL;
    gTasks[NUM_TASKS - 1].next = TAIL_SENTINEL;
}

/**
 * FUNCTION: CreateTask
 *
 * PURPOSE: Register a new task function to be called every frame.
 *
 * HOW IT WORKS:
 * Finds the first inactive slot in gTasks[], sets up the function
 * and priority, inserts it into the priority-sorted linked list,
 * zeros the data array, and marks it active.
 *
 * @param func     — The function to call each frame. Signature: void func(u8 taskId)
 * @param priority — Execution order (0 = runs first, 255 = runs last)
 *
 * RETURNS: The task ID (0-15), or 0 if all slots are full.
 */
u8 CreateTask(TaskFunc func, u8 priority)
{
    u8 i;

    for (i = 0; i < NUM_TASKS; i++)
    {
        if (!gTasks[i].isActive)
        {
            gTasks[i].func = func;
            gTasks[i].priority = priority;
            InsertTask(i);                         /* Insert into sorted linked list */
            memset(gTasks[i].data, 0, sizeof(gTasks[i].data));
            gTasks[i].isActive = TRUE;
            return i;
        }
    }

    return 0;  /* All 16 slots are in use - this is a bug condition */
}

/**
 * FUNCTION: InsertTask
 *
 * PURPOSE: Insert a task into the priority-sorted linked list.
 *
 * HOW IT WORKS:
 * Walks the linked list from the head (highest priority / lowest number).
 * Finds the first existing task with a HIGHER priority number (= lower
 * execution priority), and inserts the new task BEFORE it.
 * If no such task exists, the new task is appended at the end.
 *
 * This maintains the invariant: tasks earlier in the list have
 * lower priority numbers and run first in RunTasks().
 *
 * @param newTaskId — Array index of the task to insert
 */
static void InsertTask(u8 newTaskId)
{
    u8 taskId = FindFirstActiveTask();

    if (taskId == NUM_TASKS)
    {
        /* No other active tasks exist - this is the only task */
        gTasks[newTaskId].prev = HEAD_SENTINEL;
        gTasks[newTaskId].next = TAIL_SENTINEL;
        return;
    }

    while (1)
    {
        if (gTasks[newTaskId].priority < gTasks[taskId].priority)
        {
            /*
             * Found a task with a higher priority NUMBER (= lower priority).
             * Insert the new task BEFORE this one in the list.
             * This involves updating 3 pointers: new->prev, new->next,
             * and the previous task's next (if it exists).
             */
            gTasks[newTaskId].prev = gTasks[taskId].prev;
            gTasks[newTaskId].next = taskId;
            if (gTasks[taskId].prev != HEAD_SENTINEL)
                gTasks[gTasks[taskId].prev].next = newTaskId;
            gTasks[taskId].prev = newTaskId;
            return;
        }
        if (gTasks[taskId].next == TAIL_SENTINEL)
        {
            /* Reached the end of the list without finding a lower-priority task.
             * Append the new task at the tail. */
            gTasks[newTaskId].prev = taskId;
            gTasks[newTaskId].next = gTasks[taskId].next;
            gTasks[taskId].next = newTaskId;
            return;
        }
        taskId = gTasks[taskId].next;
    }
}

/**
 * FUNCTION: DestroyTask
 *
 * PURPOSE: Remove a task from the active list so it stops executing.
 *
 * HOW IT WORKS:
 * Standard doubly-linked list node removal. Updates the prev/next pointers
 * of neighboring tasks to skip over the removed task. Handles edge cases
 * for head and tail positions.
 *
 * The task slot remains in the array but isActive=FALSE, so CreateTask
 * can reuse it later.
 *
 * @param taskId — The task ID (0-15) returned by CreateTask
 */
void DestroyTask(u8 taskId)
{
    if (gTasks[taskId].isActive)
    {
        gTasks[taskId].isActive = FALSE;

        if (gTasks[taskId].prev == HEAD_SENTINEL)
        {
            /* Removing the head of the list */
            if (gTasks[taskId].next != TAIL_SENTINEL)
                gTasks[gTasks[taskId].next].prev = HEAD_SENTINEL;
        }
        else
        {
            if (gTasks[taskId].next == TAIL_SENTINEL)
            {
                /* Removing the tail of the list */
                gTasks[gTasks[taskId].prev].next = TAIL_SENTINEL;
            }
            else
            {
                /* Removing from the middle - bridge the gap */
                gTasks[gTasks[taskId].prev].next = gTasks[taskId].next;
                gTasks[gTasks[taskId].next].prev = gTasks[taskId].prev;
            }
        }
    }
}

/**
 * FUNCTION: RunTasks
 *
 * PURPOSE: Execute all active tasks in priority order.
 *
 * HOW IT WORKS:
 * Finds the head of the linked list (the task with HEAD_SENTINEL as prev),
 * then walks the list calling each task's function. Each function receives
 * its own task ID so it can access its data[] and destroy itself.
 *
 * Called once per frame from the game's main VBlank callback.
 * Typically this is called from within the VBlank callback set by each
 * game state (overworld, battle, menu, etc.).
 */
void RunTasks(void)
{
    u8 taskId = FindFirstActiveTask();

    if (taskId != NUM_TASKS)
    {
        do
        {
            gTasks[taskId].func(taskId);    /* Call this task's function */
            taskId = gTasks[taskId].next;   /* Advance to next task in priority order */
        } while (taskId != TAIL_SENTINEL);
    }
}

/**
 * FUNCTION: FindFirstActiveTask
 *
 * PURPOSE: Find the task at the head of the priority-sorted linked list.
 *
 * HOW IT WORKS:
 * Scans the array for a task that is both active AND has HEAD_SENTINEL
 * as its prev pointer (meaning it's the first in the linked list).
 * Returns NUM_TASKS if no active tasks exist.
 *
 * RETURNS: Task ID of the head task, or NUM_TASKS (16) if none active
 */
static u8 FindFirstActiveTask()
{
    u8 taskId;

    for (taskId = 0; taskId < NUM_TASKS; taskId++)
        if (gTasks[taskId].isActive == TRUE && gTasks[taskId].prev == HEAD_SENTINEL)
            break;

    return taskId;
}

/**
 * FUNCTION: TaskDummy
 *
 * PURPOSE: Placeholder function assigned to inactive tasks. Does nothing.
 */
void TaskDummy(u8 taskId)
{
}

/**
 * FUNCTION: SetTaskFuncWithFollowupFunc
 *
 * PURPOSE: Change a task's current function and store a "followup" function
 *          to switch to later.
 *
 * HOW IT WORKS:
 * The followup function pointer (32 bits) is split into two 16-bit halves
 * and stored in the last two data[] slots (indices 14 and 15).
 * This is a clever trick to store a function pointer in the data array
 * which only holds s16 values.
 *
 * Pattern: A task runs 'func' for a while, then calls SwitchTaskToFollowupFunc
 * to transition to 'followupFunc' without creating a new task.
 *
 * @param taskId       — Task to modify
 * @param func         — New current function
 * @param followupFunc — Function to switch to later
 */
void SetTaskFuncWithFollowupFunc(u8 taskId, TaskFunc func, TaskFunc followupFunc)
{
    u8 followupFuncIndex = NUM_TASK_DATA - 2;

    /* Store the 32-bit function pointer as two 16-bit halves.
     * data[14] = low 16 bits of the address
     * data[15] = high 16 bits of the address */
    gTasks[taskId].data[followupFuncIndex] = (s16)((u32)followupFunc);
    gTasks[taskId].data[followupFuncIndex + 1] = (s16)((u32)followupFunc >> 16);
    gTasks[taskId].func = func;
}

/**
 * FUNCTION: SwitchTaskToFollowupFunc
 *
 * PURPOSE: Switch a task to the followup function that was previously stored.
 *
 * HOW IT WORKS:
 * Reconstructs the function pointer from the two s16 values stored in
 * data[14] and data[15], then sets that as the task's current function.
 *
 * @param taskId — Task to switch
 */
void SwitchTaskToFollowupFunc(u8 taskId)
{
    u8 followupFuncIndex = NUM_TASK_DATA - 2;

    /* Reconstruct the 32-bit pointer from two 16-bit halves */
    gTasks[taskId].func = (TaskFunc)((u16)(gTasks[taskId].data[followupFuncIndex]) | (gTasks[taskId].data[followupFuncIndex + 1] << 16));
}

/**
 * FUNCTION: FuncIsActiveTask
 *
 * PURPOSE: Check if any active task is running a specific function.
 *
 * GAME LOGIC:
 * Used to check if a particular operation is still in progress.
 * For example, before starting a screen fade, check if a previous
 * fade task is still running.
 *
 * @param func — Function pointer to search for
 * RETURNS: TRUE if any active task has this function, FALSE otherwise
 */
bool8 FuncIsActiveTask(TaskFunc func)
{
    u8 i;

    for (i = 0; i < NUM_TASKS; i++)
        if (gTasks[i].isActive == TRUE && gTasks[i].func == func)
            return TRUE;

    return FALSE;
}

/**
 * FUNCTION: FindTaskIdByFunc
 *
 * PURPOSE: Find the task ID of the first active task running a specific function.
 *
 * @param func — Function pointer to search for
 * RETURNS: Task ID (0-15) if found, or 0xFF (-1 as u8) if not found
 */
u8 FindTaskIdByFunc(TaskFunc func)
{
    s32 i;

    for (i = 0; i < NUM_TASKS; i++)
        if (gTasks[i].isActive == TRUE && gTasks[i].func == func)
            return (u8)i;

    return -1;
}

/**
 * FUNCTION: GetTaskCount
 *
 * PURPOSE: Count the number of currently active tasks.
 *
 * RETURNS: Number of active tasks (0-16)
 */
u8 GetTaskCount(void)
{
    u8 i;
    u8 count = 0;

    for (i = 0; i < NUM_TASKS; i++)
        if (gTasks[i].isActive == TRUE)
            count++;

    return count;
}

/**
 * FUNCTION: SetWordTaskArg
 *
 * PURPOSE: Store a 32-bit value in two consecutive data[] slots.
 *
 * HOW IT WORKS:
 * Since data[] elements are s16 (16-bit), a 32-bit value needs two slots.
 * The low 16 bits go in data[dataElem], the high 16 bits in data[dataElem+1].
 * Guards against writing past the array bounds (max index 14, since we need 2 slots).
 *
 * @param taskId   — Task ID
 * @param dataElem — Starting data index (0-14)
 * @param value    — 32-bit value to store
 */
void SetWordTaskArg(u8 taskId, u8 dataElem, unsigned long value)
{
    if (dataElem <= 14)
    {
        gTasks[taskId].data[dataElem] = value;           /* Low 16 bits */
        gTasks[taskId].data[dataElem + 1] = value >> 16; /* High 16 bits */
    }
}

/**
 * FUNCTION: GetWordTaskArg
 *
 * PURPOSE: Retrieve a 32-bit value from two consecutive data[] slots.
 *
 * @param taskId   — Task ID
 * @param dataElem — Starting data index (0-14)
 * RETURNS: The reconstructed 32-bit value, or 0 if dataElem > 14
 */
u32 GetWordTaskArg(u8 taskId, u8 dataElem)
{
    if (dataElem <= 14)
        return (u16)gTasks[taskId].data[dataElem] | (gTasks[taskId].data[dataElem + 1] << 16);
    else
        return 0;
}
