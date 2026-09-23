// doc: mutex
#include "burrow/sync.h"

static SyncMutex mu;
static int balance;

void deposit(int n) {
    sync_mutex_lock(&mu);
    balance += n;
    sync_mutex_unlock(&mu);
}
// doc: end

#include <stdio.h>

#include "burrow/burrow.h"

static SyncRWMutex table;
static int scans;

static void report(int n) {
    printf("balance is %d\n", n);
}

static void report_busy(void) {
    printf("busy\n");
}

static void check_balance(void) {
    // doc: try-lock
    if (sync_mutex_try_lock(&mu)) {
        report(balance);
        sync_mutex_unlock(&mu);
    } else {
        report_busy();
    }
    // doc: end
}

static void work(void *env) {
    (void)env;
    balance += 10;
}

static void scan(void *env) {
    (void)env;
    scans++;
}

// doc: locker
void with_lock(SyncLocker l, Func body) {
    sync_locker_lock(l);
    BURROW_CALLF0(body);
    sync_locker_unlock(l);
}

void update_and_scan(void) {
    with_lock(sync_mutex_locker(&mu), BURROW_FN(Func, work, NULL));
    with_lock(sync_rw_mutex_r_locker(&table), BURROW_FN(Func, scan, NULL));
}
// doc: end

static void local_cond(void) {
    // doc: cond-local
    SyncCond ready = SYNC_COND(sync_mutex_locker(&mu));
    // doc: end
    sync_cond_broadcast(&ready);
    printf("a broadcast with nobody waiting is fine\n");
}

int main(void) {
    deposit(5);
    check_balance();
    sync_mutex_lock(&mu);
    check_balance();
    sync_mutex_unlock(&mu);
    update_and_scan();
    printf("balance %d after %d scan\n", balance, scans);
    local_cond();
    return 0;
}

/* Output:
balance is 5
busy
balance 15 after 1 scan
a broadcast with nobody waiting is fine
*/
