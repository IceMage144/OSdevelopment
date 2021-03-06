#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "process.h"
#include "error.h"
#include "utilities.h"
#include "queue.h"
#include "stack.h"

#define QUANTUM_VAL 1.0
#define CPU_CORE 1


static Timer timer; // global timer
static pthread_t **ranThreads; // Storages the threads that we run
static pthread_mutex_t gmtx; // global mutex
static double *priority; // array with the priority of each process
static int finished = 0; // number of finished processes
static double var = 0;
static double avg = 0;
static int count = 0;

static bool* firstTime;

static void *iWait(void *);
static double calcQuanta(double);
static void *run(void *);
static double calculatePriority(Process p);
static void addToStats(double priority);
static void removeFromStats(double);
static void wakeup_next(Queue, Stack *);

void schedulerPriority(ProcArray pQueue){
    Node *tmp;
    pthread_t idleThread;
    int sz = pQueue->i + 1; // size auxiliar variable
    Stack *pool = new_stack(pQueue->i); // Pool for arriving processes
    Queue runningP = new_queue(); // Queue of processes to run at the moment
    priority = emalloc(sizeof(double)*sz); // Array with the priority of each process
    ranThreads = emalloc(sizeof(pthread_t*)*sz);
    firstTime = emalloc(sizeof(bool)*sz);
    for(int i = 0; i < sz; firstTime[i] = true,  i++);

    timer = new_Timer();
    bool notIdle = true;

    // Transfer processes to stack pool
    for (int i = pQueue->i - 1; i >= 0; i--)
        pool->v[pQueue->i - i - 1].p = &(pQueue->v[i]);

    pthread_mutex_init(&gmtx, NULL);
    pthread_mutex_lock(&gmtx);

    // Initiate all the mutexes
    for (int i = 0; i < pQueue->i; i++) {
        pthread_mutex_init(&(pool->v[i].mtx), NULL);
        pthread_mutex_lock(&(pool->v[i].mtx));
    }

    wakeup_next(runningP, pool);

    while (finished < pQueue->i) {
        if (!queue_first(runningP)) {
            if (!(tmp = stack_top(pool)))
                break;
            // Wait in idle mode if queue is empty
            double wt = tmp->p->t0 - timer->passed(timer);
            ranThreads[0] = &idleThread;
            notIdle = false;
            pthread_create(&idleThread, NULL, &iWait, &wt);
        }
        pthread_mutex_lock(&gmtx);
        wakeup_next(runningP, pool);
    }

    // Freeing all threads...
    for(int i = notIdle; i < sz; i++)
        if(ranThreads[i] != NULL)
            pthread_join(*ranThreads[i],NULL);
    free(ranThreads);
    free(runningP);
    free(pool->v);
    free(pool);
    free(priority);
    destroy_Timer(timer);

    write_outfile("%d\n", get_ctx_changes());
}

/*
 * Function: run
 * --------------------------------------------------------
 * The function that each process run. The processes will run
 * this for the quanta calculated for the processes, base on
 * the priority previously calculated.
 * When it has slept for this calculated time, it will unlock
 * the thread, for other processes to run.
 *
 * @args arg : the node of the process
 *
 * @return
 */
static void *run(void *arg) {
    Node *n = (Node *)arg;
    double w;

    do {
        pthread_mutex_lock(&(n->mtx));
        debugger(RUN_EVENT, n->p, CPU_CORE);
        if(firstTime[n->p->nLine]){
            firstTime[n->p->nLine] = false;
        }
        // It will always run the minimum to complete, to not waste cpu time
        w = fmin(n->p->dt, calcQuanta(priority[n->p->nLine]));
        sleepFor(w);
        n->p->dt -= w;
        debugger(EXIT_EVENT, n->p, CPU_CORE);
        pthread_mutex_unlock(&gmtx);
    } while (n->p->dt);

    finished++;
    debugger(END_EVENT, n->p, finished);
    write_outfile("%s %lf %lf\n", n->p->name, timer->passed(timer), timer->passed(timer) - n->p->t0);

    return NULL;
}

/*
 * Function: calculatePriority
 * --------------------------------------------------------
 * Calculate the priority of a given process. The function to
 * calculate the priority of the process depends on the t0
 * of the process, the dt of the processes, and on a new value
 * called 'punctuality', which is how much time more we have to
 * complete the process until the deadline, or the "tightness"
 * of this interval. As we choose this to be a very important factor,
 * the priority depends linearly and quadratically on this factor.

 * @args  p :  a process
 *
 * @return  a double representing the priority of the process. The
 *          lower this number, the more priority it will have to run.
 */
static double calculatePriority(Process p){
    // Initialize the priority with a high value
    double priority = 5000;
    double t0 = p.t0;
    double dt = p.dt;
    double punc = p.deadline - p.dt;
    /* These constants were calculated using machine learning
     * and gradient descent on a base of values that we estipulated.
     * The graph of this function can be visualized in the documents.
    */
    double d = 0.207715732988;
    double c = 0.21137699282;
    double b = 2.06241892813;
    double a = 0.00213475762298;
    /* If a process has punc < 0, it's impossible to complete it
     * before it's deadline, so we give it the lowest priority to
     * run (the 5000 that we initialized)
     */
    if(punc > 0)
        priority = a*pow(punc, 2) + b*punc + c*dt + d*t0;
    return priority;
}

/*
 * Function: calcQuanta
 * --------------------------------------------------------
 * Calculate how much quanta should be given to a process
 * based on its priority and the average and variance of
 * all running processes priorities. It assumes that the
 * priorities follow a normal distribution (which I'm not
 * sure), and, even if they doesn't, its a good dinamic
 * measuring system. This gives from 1 to 10 quanta.
 *
 * @args priority : process priority
 *
 * @return how much quanta the process will have this turn
 */
static double calcQuanta(double priority) {
    double L = (!var)? 0 : (priority - avg)/sqrt(var);
    double scale = 2.25*fmin(4.0, fabs(L));
    return QUANTUM_VAL*(scale + 1);
}

/*
 * Function: addToStats
 * --------------------------------------------------------
 * Add the new priority to global average and variance of
 * running processes
 *
 * @args priority : new process priority
 *
 * @return
 */
static void addToStats(double priority) {
    var = (count*(var + pow(avg, 2)) + pow(priority, 2))/(count + 1);
    avg = (avg*count + priority)/(count + 1);
    var -= pow(avg, 2);
    if (isnan(var))
        var = 0;
    count++;
}

/*
 * Function: removeFromStats
 * --------------------------------------------------------
 * Remove the priority of a finished process from global
 * average and variance of running processes
 *
 * @args priority : finished process priority
 *
 * @return
 */
static void removeFromStats(double priority) {
    if (count == 1) {
        var = 0;
        avg = 0;
    }
    else {
        var = (count*(var + pow(avg, 2)) - pow(priority, 2))/(count - 1);
        avg = (avg*count - priority)/(count - 1);
        var -= pow(avg, 2);
    }
    count--;
}

/*
 * Function: iWait
 * --------------------------------------------------------
 * Sleep scheduler for a given time
 *
 * @args  t :  a double *, the time in seconds to wait
 *
 * @return
 */
static void *iWait(void *t) {
    double *dt = (double *)t;
    sleepFor(*dt);
    pthread_mutex_unlock(&gmtx);
    return NULL;
}

/*
 * Function: wakeup_next
 * --------------------------------------------------------
 * Add new processes to queue and wake up processes from queue
 *
 * @args queue : process queue
 *       stack : not-iet-arrived process stack
 *
 * @return
 */
static void wakeup_next(Queue q, Stack *s){
    Node *n = stack_top(s);
    Node *mem = NULL;
    Node *notEmpty = queue_first(q);
    while (n && n->p->t0 <= timer->passed(timer)) {
        // set priority of process in quantum array
        priority[n->p->nLine] = calculatePriority(*(n->p));
        addToStats(priority[n->p->nLine]);
        // Add new processes to queue if global time > t0
        queue_add(q, n);
        debugger(ARRIVAL_EVENT, n->p, 0);
        stack_remove(s);
        ranThreads[n->p->nLine] = &(n->t);
        pthread_create(&(n->t), NULL, &run, (void *)n);
        n = stack_top(s);
    }
    // Readd the process to queue or remove it from queue
    if (notEmpty) {
        if ((mem = queue_first(q)) && mem->p->dt)
            queue_readd(q);
        else if (mem){
            queue_remove(q);
            removeFromStats(priority[mem->p->nLine]);
        }
    }

    // Start/restart the next process
    if ((n = queue_first(q)))
        pthread_mutex_unlock(&(n->mtx));
    if (mem != n && n)
        debugger(CONTEXT_EVENT, NULL, 0);
}
