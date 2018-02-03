#include "parsegraph_environment.h"
#include "parsegraph_environment.h"
#include <parsegraph_user.h>
#include <stdio.h>
#include <http_log.h>

#include <parsegraph_List.h>

int USER_ID = 10;

static apr_pool_t* pool = NULL;
static ap_dbd_t* dbd;

void ap_log_perror(
    const char *  	file,
    int  	line,
    int  	module_index,
    int  	level,
    apr_status_t  	status,
    apr_pool_t *  	p,
    const char *  	fmt,
    ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

int main(int argc, const char* const* argv)
{
    // Initialize the APR.
    apr_status_t rv;
    rv = apr_app_initialize(&argc, &argv, NULL);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed initializing APR. APR status of %d.\n", rv);
        return -1;
    }
    rv = apr_pool_create(&pool, NULL);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed creating memory pool. APR status of %d.\n", rv);
        return -1;
    }

    // Initialize DBD.
    rv = apr_dbd_init(pool);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed initializing DBD, APR status of %d.\n", rv);
        return -1;
    }
    dbd = (ap_dbd_t*)apr_palloc(pool, sizeof(ap_dbd_t));
    if(dbd == NULL) {
        fprintf(stderr, "Failed initializing DBD memory");
        return -1;
    }
    rv = apr_dbd_get_driver(pool, "sqlite3", &dbd->driver);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed creating DBD driver, APR status of %d.\n", rv);
        return -1;
    }
    const char* db_path = "/home/dafrito/var/parsegraph/users.sqlite";
    rv = apr_dbd_open(dbd->driver, pool, db_path, &dbd->handle);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed connecting to database at %s, APR status of %d.\n", db_path, rv);
        return -1;
    }
    dbd->prepared = apr_hash_make(pool);

    rv = parsegraph_upgradeUserTables(pool, dbd);
    if(rv != 0) {
        fprintf(stderr, "Failed installing user tables, status of %d.\n", rv);
        return -1;
    }

    rv = parsegraph_List_upgradeTables(pool, dbd);
    if(rv != 0) {
        fprintf(stderr, "Failed installing list tables, status of %d.\n", rv);
        return -1;
    }

    parsegraph_EnvironmentStatus erv;
    erv = parsegraph_upgradeEnvironmentTables(pool, dbd);
    if(erv != 0) {
        fprintf(stderr, "Failed installing environment tables, status of %d.\n", erv);
        return -1;
    }

    rv = parsegraph_prepareLoginStatements(pool, dbd);
    if(rv != 0) {
        fprintf(stderr, "Failed preparing SQL statements, status of %d.\n", rv);
        return -1;
    }

    rv = parsegraph_List_prepareStatements(pool, dbd);
    if(rv != 0) {
        fprintf(stderr, "Failed preparing SQL statements, status of %d.\n", rv);
        return -1;
    }

    erv = parsegraph_prepareEnvironmentStatements(pool, dbd);
    if(erv != parsegraph_Environment_OK) {
        return -1;
    }

    parsegraph_GUID env;
    switch(parsegraph_createEnvironment(pool, dbd, USER_ID, 0, 0, &env)) {
    case parsegraph_Environment_OK:
        break;
    default:
        return -1;
    }

    // Close the DBD connection.
    rv = apr_dbd_close(dbd->driver, dbd->handle);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed closing database, APR status of %d.\n", rv);
        return -1;
    }

    // Destroy the pool for cleanliness.
    apr_pool_destroy(pool);
    dbd = NULL;
    pool = NULL;

    apr_terminate();
}

