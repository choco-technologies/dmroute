#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmroute.h"

static dmroute_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmroute_create();
}

void dmod_test_teardown(void)
{
    dmroute_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmroute_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmroute_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmroute_is_valid(g_handle));
}

DMOD_TEST_STEP(dmroute_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmroute_destroy(NULL);
}
